# Perfil de transporte USB HID

## 1. Escopo e termos normativos

Este documento define como frames BTP atravessam uma interface USB HID
vendor-defined entre um dispositivo e o host. O envelope, o CRC e a
fragmentação continuam regidos por [`BTP_V1.md`](BTP_V1.md).

As convenções normativas deste documento estão em
[`CONVENTIONS.md`](CONVENTIONS.md), inclusive a distinção de três níveis
entre "aceito", "confirmado no enlace" e "concluído". Neste perfil, "aceito"
é a API HID local (`SendReport`/`hid_write`) ter aceitado a escrita e
"confirmado no enlace" é a transferência USB ter completado sem erro de
transporte -- a mesma distinção usada em
[`TRANSPORT_ESPNOW.md`](TRANSPORT_ESPNOW.md) §1.

Este perfil coexiste com `Serial` (`TRANSPORT_SERIAL.md`) no mesmo
dispositivo composto: ambas as interfaces permanecem ativas simultaneamente,
cada uma com seu próprio `source_id`/`boot_id`/sessão BTP independentes. Um
cliente escolhe qual interface usar por conexão; este documento não define
nenhuma forma de migrar uma sessão de uma interface para outra.

## 2. Mapeamento para relatórios HID

Cada relatório HID físico tem exatamente 64 octetos: 1 octeto de Report ID
seguido de 63 octetos de dados. Um relatório HID de tamanho fixo **sempre**
transmite os 63 octetos completos -- uma escrita menor que isso é preenchida
com zeros pela pilha USB (TinyUSB/`USBHIDVendor`) antes do envio, porque o
endpoint de interrupt não tem como transmitir um relatório parcial. Sem mais
nenhuma informação, o lado que recebe não teria como distinguir dado real de
padding de zeros à direita.

Por isso este perfil reserva o primeiro octeto dos 63 como um **prefixo de
tamanho** (`USBHIDVendor(report_size, prepend_size=true)` do lado do
firmware): esse octeto informa quantos dos octetos seguintes são dado válido
(0 a 62); o restante do relatório é padding e **MUST** ser ignorado pelo
receptor. Os 62 octetos remanescentes são dedicados inteiramente ao frame BTP:

```text
relatorio HID = report_id (1) || tamanho_valido (1) || frame BTP (ate 62, resto e padding)
```

Um relatório **MUST** conter no máximo um frame BTP completo -- ou um
fragmento, que já é por si só um frame completo (`BTP_V1.md` §5). Diferente da
serial, não há COBS nem delimitador dentro dos 62 octetos de dado: o próprio
relatório (mais o prefixo de tamanho) já é a unidade de framing, entregue como
uma transferência de interrupt discreta pelo host USB. Um relatório **MUST
NOT** conter mais de um frame nem um pedaço de frame.

O BTP adota o limite realmente suportado por um relatório HID Full-Speed
com esse prefixo:

| Limite | Valor |
| --- | ---: |
| `BTP_USB_HID_MAX_FRAME_SIZE` | 62 octetos |
| `BTP_USB_HID_MAX_PAYLOAD_SIZE` | 22 octetos |

O transmissor **MUST** rejeitar um frame maior antes de escrever no relatório,
e **MUST** escrever o octeto de tamanho válido corretamente (o firmware faz
isso automaticamente via `USBHIDVendor::write()` com `prepend_size=true`; um
cliente desktop usando `hidapi` **MUST** replicar o mesmo prefixo ao montar o
relatório OUT). O receptor **MUST** truncar o relatório recebido no tamanho
válido declarado antes de tratar o restante como dado, depois rejeitar um
frame BTP cujo tamanho declarado (`payload_size` do header) seja menor que 40
ou maior que 62 octetos e exigir que seu tamanho bata exatamente com o
declarado no header BTP. Um dispositivo USB Full-Speed **MUST NOT** anunciar
nem depender de um endpoint maior que 64 octetos para este perfil; High-Speed,
um report maior ou uma convenção de framing diferente **MAY** ser adotados por
uma extensão futura negociada, nunca implicitamente.

## 3. Fragmentação e ordenação

Uma mensagem lógica com mais de 22 octetos de payload **MUST** usar a
fragmentação de `BTP_V1.md`, com os mesmos `fragment_count`/`make_fragment` já
compartilhados por ESP-NOW e Serial (`btp::fragment_count`/`make_fragment` são
parametrizados por `TransportProfile` e não mudam de comportamento entre
perfis, só a constante de limite usada). Por causa do teto de 22 octetos, até
mesmo mensagens de controle pequenas como `HELLO` **MUST** esperar fragmentar
em varios relatorios -- isso é uma característica normal deste perfil, não uma
falha de negociação.

O transporte USB HID entrega relatórios na ordem em que foram transmitidos
para o mesmo endpoint (garantia do próprio USB para um pipe de interrupt), mas
isso **MUST NOT** ser presumido pelo reassembly: a mesma tolerância a perda,
duplicação e fragmentos fora de ordem exigida para ESP-NOW e Serial se aplica
aqui, já que uma reconexão, um relatório descartado pelo driver de host ou uma
nova sessão pode reordenar ou repetir fragmentos do ponto de vista do
receptor. Nenhum fragmento reserva o enlace até o fim da mensagem lógica.

## 4. Ciclo de uma tentativa de envio

Para cada relatório, a implementação distingue obrigatoriamente:

1. `SendReport`/`hid_write` retorna falha: a tentativa foi rejeitada
   localmente e não está pendente;
2. `SendReport`/`hid_write` retorna sucesso: os bytes foram aceitos pela pilha
   USB local, sem garantia de que o host os processou;
3. a transferência de interrupt completa sem erro no lado do host (ACK USB):
   houve confirmação no enlace, mas não confirma decode, roteamento ou
   execução;
4. timeout de `SendReport` sem ACK do host, ou erro de transporte reportado
   pela pilha USB: a entrega permanece não confirmada.

Diferente de ESP-NOW, este enlace é ponto a ponto (um dispositivo, um host) e
não tem conceito de peer/MAC: cada relatório vai para o único endpoint HID
ativo.
Diferente da serial, não há conceito de baud nem de line coding -- a
capacidade do enlace é fixa pela topologia do barramento USB, não configurada
pelo firmware.

## 5. Confirmação, retry e deduplicação

USB HID não acrescenta uma mensagem ACK ao BTP. A confiabilidade fim a fim é
definida pelo tipo lógico, mesma tabela de `TRANSPORT_ESPNOW.md` §5:

| Tráfego | Confirmação fim a fim | Política de retry |
| --- | --- | --- |
| `COMMAND_REQUEST` | `COMMAND_RESULT` referenciando a requisição | Retry limitado, com a mesma identidade e os mesmos bytes |
| Controle que define mensagem de resultado | Resultado correspondente | Mesma regra da operação |
| `TELEMETRY` | Nenhuma por amostra | Nunca retransmitir a amostra |
| `LOG`, `STATUS`, `TERMINAL_OUT` espontâneo | Nenhuma, salvo regra futura explícita | Best effort |

Um erro de escrita local **MAY** antecipar uma nova tentativa de comando; um
`SendReport` bem-sucedido **MUST NOT** encerrar a espera pelo resultado BTP.
Retry é limitado por prazo e quantidade configurados; ao esgotá-los, a
operação falha explicitamente.

## 6. Filas e congestionamento

O scheduler **MUST** manter filas lógicas separadas e respeitar a ordem de
prioridade definida em `COMMANDS_AND_ACTIONS.md`, mesma regra de ESP-NOW e
Serial:

- comandos, resultados e sessão não aguardam a drenagem de telemetria;
- fila cheia de telemetria descarta telemetria, preferindo a amostra mais
  recente quando a semântica do tópico permitir;
- um `SendReport` pendente tem timeout finito e não bloqueia indefinidamente o
  laço principal do firmware;
- FIFO é preservado dentro da mesma classe, ressalvado o descarte documentado
  de telemetria obsoleta.

## 7. Sessão sempre protocolada

Diferente da serial (`TRANSPORT_SERIAL.md` §5), a interface HID **MUST NOT**
oferecer um modo console humano nem a troca textual `BTP/1 ENTER`/`READY`:
nenhum operador digita em um endpoint HID vendor-defined. A interface **MUST**
estar pronta para receber `HELLO` assim que o host a abre, sem negociação de
modo prévia -- mesmo espírito de ESP-NOW, que também não tem console.

O primeiro frame do cliente **MUST** ser `HELLO` (possivelmente fragmentado,
ver §3) e chegar em até 2000 ms depois da interface aberta pelo host. As
versões e capacidades efetivas só valem depois de `HELLO_RESULT=SUCCESS`.
`SESSION_CLOSE`/`SESSION_CLOSE_RESULT` ou o watchdog de `session_timeout_ms`
negociado encerram a sessão; ao encerrar, a interface volta a esperar um novo
`HELLO`, sem nenhum estado de "console" para retornar.

## 8. Casos mínimos de conformidade

Uma implementação deste transporte deve demonstrar:

- frame de 40 octetos (payload vazio) e de 62 octetos (payload máximo de 22);
- rejeição de um frame de 63 octetos antes mesmo de ler `payload_size`;
- truncamento correto do relatório recebido pelo prefixo de tamanho antes de
  tratar o restante como padding;
- fragmentação de uma mensagem lógica de 50 octetos em 3 relatórios
  (22 + 22 + 6);
- payload contendo `00 0a 0d 7f 80 ff` recebido byte a byte sem alteração;
- distinção entre retorno de `SendReport`/`hid_write` e confirmação de ACK do
  host;
- `HELLO` fragmentado com sucesso quando maior que 22 octetos de payload;
- fragmentos perdidos, duplicados e fora de ordem sem entrega parcial, mesma
  garantia de `TRANSPORT_ESPNOW.md`.
