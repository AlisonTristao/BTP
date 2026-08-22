# Perfil de transporte ESP-NOW

## 1. Escopo e termos normativos

Este documento define como frames BTP atravessam o enlace ESP-NOW entre um
produtor e um gateway. O envelope, o CRC e a fragmentação continuam regidos
por [`BTP_V1.md`](BTP_V1.md).

As convenções normativas deste documento estão em
[`CONVENTIONS.md`](CONVENTIONS.md), inclusive a distinção entre "aceito"
(a API local aceitou a tentativa de envio), "confirmado no enlace" (o
callback de envio reportou sucesso) e "concluído" (o protocolo de aplicação
produziu a resposta esperada). Esses três eventos não são equivalentes, e
este capítulo depende dessa diferença o tempo todo.

## 2. Mapeamento para datagramas

Cada datagrama ESP-NOW **MUST** conter exatamente um frame BTP completo:

```text
datagrama ESP-NOW = header BTP || payload deste fragmento || CRC32
```

Um fragmento de mensagem lógica já é um frame BTP completo e, portanto,
ocupa sozinho um datagrama. Um datagrama **MUST NOT** conter mais de um frame,
um pedaço de frame, prefixo de tamanho, delimitador, estrutura C/C++ ou
padding. Seu tamanho é exatamente `40 + payload_size`.

O BTP adota o limite comum realmente suportado pelos firmwares atuais:

| Limite | Valor |
| --- | ---: |
| `BTP_ESPNOW_MAX_FRAME_SIZE` | 250 octetos |
| `BTP_ESPNOW_MAX_PAYLOAD_SIZE` | 210 octetos |

O transmissor **MUST** rejeitar um frame maior antes de chamar `esp_now_send`.
O receptor **MUST** rejeitar um datagrama menor que 40 ou maior que 250
octetos e, depois, exigir que seu tamanho seja exatamente o declarado no
header BTP. A integração com ESP-IDF **SHOULD** verificar em compilação que
o limite configurado não excede `ESP_NOW_MAX_DATA_LEN`.

Versões de ESP-NOW capazes de datagramas maiores não aumentam implicitamente
o limite do BTP. Usá-las exige uma extensão negociada ou nova versão do
contrato que todos os peers do caminho suportem.

O segundo vetor de `BTP_V1.md`, cujo payload é `00 0a 0d ff`, ocupa um
datagrama de 44 octetos. O ESP-NOW recebe os 44 octetos do frame sem qualquer
transformação; zero, LF e CR não têm significado especial neste transporte.

## 3. Fragmentação e ordenação

Uma mensagem lógica com mais de 210 octetos de payload **MUST** usar a
fragmentação de `BTP_V1.md`. Cada fragmento preserva a identidade da mensagem
e possui CRC próprio. O transporte não garante que datagramas cheguem, cheguem
uma só vez ou cheguem na ordem de envio; o reassembly **MUST** tolerar perda,
duplicação e fragmentos fora de ordem, sem entregar mensagem parcial.

O emissor **SHOULD** enfileirar os fragmentos de uma primeira tentativa em
ordem crescente de `fragment_index`, mas mensagens de prioridade maior
**MAY** ser intercaladas entre fragmentos. Nenhum fragmento reserva o enlace
até o fim da mensagem lógica.

## 4. Ciclo de uma tentativa de envio

Para cada datagrama unicast, a implementação distingue obrigatoriamente:

1. `esp_now_send(...) != ESP_OK`: a tentativa foi rejeitada localmente e não
   está pendente;
2. `esp_now_send(...) == ESP_OK`: os bytes foram aceitos para transmissão
   local, sem garantia de entrega;
3. callback `ESP_NOW_SEND_SUCCESS`: houve confirmação no enlace para aquele
   peer, mas não confirma CRC, decode, reassembly, roteamento ou execução;
4. callback `ESP_NOW_SEND_FAIL`, ou timeout do callback: a entrega permanece
   não confirmada.

Broadcast não fornece confirmação individual de cada destinatário. Um
callback de sucesso para broadcast **MUST NOT** ser exposto como entrega a
todos os peers.

O transmissor **MUST** correlacionar sem ambiguidade cada callback com a
tentativa pendente. Quando a API ou a versão do ESP-IDF não fornecer um
identificador suficiente, ele **MUST** limitar a concorrência (por exemplo, a
um envio pendente por peer, ou globalmente) e aguardar callback ou timeout
antes de reutilizar o mesmo slot. O callback apenas registra o resultado e
acorda o scheduler; ele **MUST NOT** bloquear, executar comando, decodificar
payload ou iniciar uma cadeia de retries.

## 5. Confirmação, retry e deduplicação

ESP-NOW não acrescenta uma mensagem ACK ao BTP. A confiabilidade fim a fim é
definida pelo tipo lógico:

| Tráfego | Confirmação fim a fim | Política de retry |
| --- | --- | --- |
| `COMMAND_REQUEST` | `COMMAND_RESULT` referenciando a requisição | Retry limitado, com a mesma identidade e os mesmos bytes |
| Controle que define mensagem de resultado | Resultado correspondente | Mesma regra da operação |
| `TELEMETRY` | Nenhuma por amostra | Nunca retransmitir a amostra |
| `LOG`, `STATUS`, `TERMINAL_OUT` espontâneo | Nenhuma, salvo regra futura explícita | Best effort |

O callback de falha **MAY** antecipar uma nova tentativa de comando; o callback
de sucesso **MUST NOT** encerrar a espera pelo resultado BTP. Ao expirar o
prazo de resposta, o solicitante **MAY** retransmitir a mensagem lógica
completa, usando os mesmos `source_id`, `boot_id`, `sequence`, request ID,
contagem de fragmentos e bytes. Ele **MUST NOT** criar nova sequência para uma
tentativa da mesma intenção. Retry é limitado por prazo e quantidade
configurados; ao esgotá-los, a operação falha explicitamente.

Se um resultado se perder, o retry da requisição permite que o executor
deduplicador devolva o resultado armazenado sem repetir o efeito. As garantias
do cache de deduplicação estão em
[`COMMANDS_AND_ACTIONS.md`](COMMANDS_AND_ACTIONS.md).

Telemetria é enviada no máximo uma vez por amostra. Rejeição local, falha no
callback, fila cheia ou perda no rádio incrementa os contadores aplicáveis e
descarta a amostra ou seus fragmentos ainda não enviados. Essa falha **MUST
NOT** criar retry, aguardar ACK ou impedir tráfego prioritário.

## 6. Filas e congestionamento

O scheduler **MUST** manter filas lógicas separadas e respeitar a ordem de
prioridade definida em `COMMANDS_AND_ACTIONS.md`. Em particular:

- comandos, resultados e sessão não aguardam a drenagem de telemetria;
- fila cheia de telemetria descarta telemetria, preferindo a amostra mais
  recente quando a semântica do tópico permitir;
- um callback pendente tem timeout finito;
- nenhum retry ocupa a thread/callback do rádio com espera ativa;
- FIFO é preservado dentro da mesma classe, ressalvado o descarte documentado
  de telemetria obsoleta.

O receptor valida o frame antes de colocá-lo em filas de canal. O MAC de origem
**MAY** ser usado para roteamento e diagnóstico, mas não substitui
`source_id`/`boot_id` nem autentica o frame.

## 7. Casos mínimos de conformidade

Uma implementação deste transporte deve demonstrar:

- frames de 40 e 250 octetos e rejeição de 251 octetos;
- payload contendo `00 0a 0d` recebido byte a byte sem alteração;
- distinção entre retorno de `esp_now_send` e callback posterior;
- callback perdido tratado por timeout finito;
- comando repetido com a mesma identidade sem repetir o efeito;
- telemetria perdida sem retry e sem bloquear um comando posterior;
- fragmentos perdidos, duplicados e fora de ordem sem entrega parcial.
