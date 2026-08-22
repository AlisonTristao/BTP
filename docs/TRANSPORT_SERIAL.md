# Perfil de transporte serial (COBS)

## 1. Escopo e termos normativos

Este documento define o framing COBS, o decoder incremental, a alternância
entre console humano e modo protocolado e a propriedade da porta serial. O
frame decodificado segue [`BTP_V1.md`](BTP_V1.md); as mensagens de sessão
seguem [`COMMANDS_AND_ACTIONS.md`](COMMANDS_AND_ACTIONS.md).

A API compartilhada que implementa COBS e os estados de recepção está descrita
em [`STREAM_AND_REASSEMBLY.md`](STREAM_AND_REASSEMBLY.md).

As convenções normativas deste documento estão em
[`CONVENTIONS.md`](CONVENTIONS.md). "Frame" abaixo é o frame BTP cru,
incluindo header, payload e CRC. "Bloco codificado" é somente o resultado
COBS, sem delimitadores.

## 2. Framing no stream

Cada frame é escrito no stream exatamente assim:

```text
pacote serial = 0x00 || COBS(frame BTP) || 0x00
```

COBS é aplicado ao frame inteiro, inclusive magic, payload e CRC. O bloco
codificado não contém `0x00`; os dois zeros pertencem somente ao transporte e
não entram no `payload_size`, no `frame_size` ou no cálculo do CRC.

Todo emissor **MUST** escrever tanto o delimitador inicial quanto o final.
Assim, pacotes consecutivos normalmente deixam dois zeros adjacentes. O
receptor **MUST** ignorar blocos vazios entre delimitadores e **MUST NOT**
entregá-los como frames BTP vazios.

Um payload pode conter zero, CR, LF ou qualquer outro octeto. Esses valores
são recuperados pelo decode COBS e nunca são interpretados como linha de
console no modo protocolado.

Por exemplo, o segundo vetor de `BTP_V1.md` tem 44 octetos e payload
`00 0a 0d ff`. Seu bloco COBS tem 45 octetos; no stream, o pacote completo é:

```text
00
04 42 54 50 04 01 01 01 02 24 02 04 19 04 03 02 01
40 30 20 10 08 07 06 05 08 07 06 05 04 03 02 01 34
12 01 02 08 0a 0d ff 2d 80 1f 40
00
```

O decode desse bloco recupera exatamente o frame original, inclusive o zero
no primeiro octeto do payload; CR e LF permanecem dados comuns.

## 3. Limites de memória

Os limites do BTP são:

| Item | Limite |
| --- | ---: |
| Frame BTP decodificado | 4096 octetos |
| Payload BTP | 4056 octetos |
| Bloco COBS, sem delimitadores | 4113 octetos |
| Pacote serial, com os dois delimitadores | 4115 octetos |

Para uma entrada de `N` octetos, o limite superior do COBS convencional é
`N + floor(N / 254) + 1`; para `N=4096`, resulta em 4113. O encoder **MUST**
calcular e validar a capacidade de saída antes de escrever.

Um decoder que acumule o bloco codificado **MUST NOT** reservar nem aceitar
mais de 4113 octetos para um candidato. Um decoder COBS realmente incremental
**MAY** usar menos memória, mas ainda **MUST** rejeitar qualquer candidato cujo
bloco codificado ultrapasse 4113 ou cuja saída ultrapasse 4096 octetos. Heap
ou crescimento dinâmico sem limite não são permitidos.

## 4. Decoder incremental e ressincronização

Ao entrar no modo protocolado, o decoder começa sem candidato e espera um
`0x00` de sincronização. Seu comportamento é equivalente a estes estados:

1. **esperando delimitador:** descarta qualquer byte até encontrar `0x00`;
2. **coletando:** acumula octetos não zero até o próximo `0x00`;
3. **descartando por overflow:** depois de exceder o limite, não acumula mais
   bytes e descarta tudo até o próximo `0x00`.

Em **coletando**, um delimitador com candidato vazio é ignorado e mantém o
decoder sincronizado. Com candidato não vazio, o receptor:

1. executa o decode COBS com limite de 4096 octetos;
2. exige um frame decodificado de 40 a 4096 octetos;
3. valida magic, versão, tamanho exato, CRC e demais invariantes BTP;
4. somente depois entrega o frame ao roteador;
5. limpa o candidato e permanece sincronizado para o bloco seguinte.

COBS inválido, frame truncado, overflow, CRC inválido ou header incoerente
descarta apenas o candidato atual. O `0x00` que o encerrou também restabelece a
sincronização; portanto, o próximo bloco válido pode ser aceito sem reiniciar a
porta. Um frame inválido **MUST NOT** renovar o watchdog da sessão.

Ruído sem delimitador é descartado no estado inicial ou leva a overflow no
estado de coleta; em ambos os casos, o próximo `0x00` recupera o decoder. Um
frame truncado seguido do delimitador inicial do próximo pacote é rejeitado,
e os bytes codificados seguintes formam um novo candidato.

## 5. Entrada no modo protocolado

A porta inicia em modo console. Nesse modo, o dispositivo reconhece como
controle somente uma linha ASCII completa com 16 dígitos hexadecimais:

```text
BTP/1 ENTER NNNNNNNNNNNNNNNN\r\n
```

Ao aceitá-la, o dispositivo termina toda saída de console já iniciada e
responde, repetindo o nonce em minúsculas:

```text
BTP/1 READY nnnnnnnnnnnnnnnn\r\n
```

O dispositivo entra no modo protocolado somente depois de escrever o `\n` final
de `READY`. Nesse instante ele limpa o estado do decoder COBS. O cliente **MUST**
esperar a linha `READY` completa antes de transmitir o delimitador inicial do
primeiro frame. Bytes recebidos entre a aceitação de `ENTER` e o fim de
`READY` **MAY** ser descartados e **MUST NOT** ser interpretados como BTP.

O primeiro frame do cliente **MUST** ser `HELLO` e chegar em até 2000 ms. As
versões e capacidades efetivas passam a valer somente após
`HELLO_RESULT=SUCCESS`. Linha inválida ou incompleta continua sendo entrada
comum do console; não existe autodetecção de frame binário.

## 6. Saída, watchdog e limpeza

O modo protocolado termina por `SESSION_CLOSE`/`SESSION_CLOSE_RESULT`, por
ausência do `HELLO` no prazo inicial ou quando nenhum frame BTP válido é
recebido durante o `session_timeout_ms` negociado. Bytes recebidos, frames
COBS inválidos e frames BTP inválidos não renovam o watchdog.

Ao encerrar, o dispositivo **MUST**:

1. parar de aceitar novo trabalho protocolado;
2. respeitar a drenagem limitada definida para `SESSION_CLOSE`;
3. descartar bloco serial parcial, reassemblies incompletos e itens ainda não
   iniciados nas filas de transmissão;
4. concluir ou abortar de forma limitada qualquer escrita já iniciada;
5. mudar a propriedade da porta para o console;
6. escrever exatamente `BTP/1 CONSOLE\r\n` como primeira saída textual.

Dados binários residuais recebidos antes da mudança de propriedade **MUST
NOT** ser entregues como comandos humanos. Fechar a sessão ou perder a serial
não limpa o cache de deduplicação de comandos nem autoriza repetir efeitos.

## 7. Propriedade exclusiva e `SerialMux`

No modo protocolado, exatamente uma instância de `SerialMux` é dona de toda
escrita na porta. Produtores de telemetria, logs, terminal, comandos e sessão
entregam ao mux frames BTP completos e uma classe de prioridade; nenhum deles
chama `Serial.write`, `printf`, logger de console ou equivalente diretamente.

O `SerialMux` **MUST**:

- validar o tamanho, codificar COBS e escrever cada pacote sem intercalar seus
  bytes com outro pacote;
- continuar uma escrita parcial da API serial antes de iniciar outro frame;
- usar filas limitadas e a prioridade de `COMMANDS_AND_ACTIONS.md`;
- aplicar backpressure ou descarte por classe, nunca bloquear comandos porque
  a fila de telemetria está cheia;
- impedir qualquer saída textual entre `READY` e `BTP/1 CONSOLE`;
- serializar também os frames gerados pelo próprio controle de sessão.

Mensagens de console produzidas durante a sessão **MAY** ser descartadas ou
convertidas explicitamente em frames `LOG`; elas **MUST NOT** vazar como texto
cru. `TERMINAL_OUT` também é um frame BTP e não concede ao terminal acesso
direto à porta física.

## 8. Baud configurada e capacidade real

O BTP não fixa uma baud universal. A baud é necessária antes que qualquer
handshake possa ser decodificado e, por isso, é configuração do enlace fora
do wire BTP. Em USB CDC, a line coding solicitada pelo host **MAY** ser apenas
informativa; isso não transforma a taxa nominal em capacidade garantida.

Cada build do firmware **MUST** ter uma única fonte de verdade para a baud.
O mesmo valor usado para inicializar a porta (por exemplo, a build flag
`BAUDRATE`) **MUST** alimentar o diagnóstico de console e os metadados do
artefato. Números copiados em documentação não são fonte de configuração.

O diagnóstico do firmware **MUST** anunciar ao menos:

- `serial.configured_baud`;
- `serial.max_decoded_frame` (4096 no wire v1);
- `serial.rx_encoded_capacity` (ao menos 4113);
- controle de fluxo habilitado, se houver.

Até existir uma extensão BTP específica para esses campos, eles pertencem ao
status do console e aos metadados de build, não a uma extensão ad hoc de
`HELLO` ou `READY`.

A capacidade sustentada **MUST** ser medida no hardware e build liberados, em
ambos os sentidos, com frames máximos e o mesmo framing/filas usados em
produção. O relatório registra baud solicitada, duração, octetos úteis por
segundo, octetos no wire por segundo, frames descartados e erros de decode/CRC.
A taxa de operação anunciada para telemetria **MUST NOT** exceder a menor
capacidade sustentada medida depois de reservar tráfego prioritário. Alterar
baud, driver, buffers, placa ou flags relevantes invalida a medição anterior.

## 9. Casos mínimos de conformidade

Uma implementação deste transporte deve demonstrar:

- round trip de payload contendo `00 0a 0d`;
- frames decodificados de 40 e 4096 octetos;
- rejeição e descarte até o delimitador após 4114 octetos codificados;
- recuperação do próximo frame após ruído, COBS inválido, CRC inválido e
  frame truncado;
- delimitadores consecutivos sem produção de frame vazio;
- nenhum texto cru ou interleaving enquanto o mux possui a porta;
- timeout retornando ao console e aceitando uma nova negociação;
- carga sustentada sem impedir a passagem de comandos prioritários.
