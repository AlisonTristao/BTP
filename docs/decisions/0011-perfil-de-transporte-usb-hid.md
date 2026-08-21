# ADR 0011: perfil de transporte USB HID

- Estado: Aceita
- Data: 2026-08-20
- Detalha: [ADR 0010](0010-perfis-de-transporte-espnow-serial.md)

## Contexto

O dongle já expõe BTP v1 sobre um único enlace físico: o periférico USB
nativo do ESP32-S3 em modo CDC, carregando o perfil `Serial` (COBS,
console/protocolo compartilhando a mesma porta, `TRANSPORT_SERIAL.md`). Esse
enlace é uma porta COM virtual: do ponto de vista do sistema operacional e de
qualquer software de terceiros, ele é indistinguível de uma UART real, com
todo o overhead de driver serial que isso implica (framing byte a byte,
COBS, sem fronteira nativa de mensagem).

Passa a existir a necessidade de um segundo canal, mais rápido e sem esse
overhead, para telemetria entre o dongle e o TraceView: uma interface USB HID
vendor-specific, que enumera nativamente em qualquer sistema operacional sem
driver dedicado e entrega dados já fragmentados em unidades discretas
(relatórios), eliminando a necessidade de COBS. O dongle passa a ser um
dispositivo USB composto: a interface CDC existente permanece inalterada,
folhado por uma segunda interface HID.

O BTP v1 hoje só reconhece dois perfis de transporte normativos, `EspNow` e
`Serial` (ADR 0010) -- `TransportProfile` é um enum usado diretamente por
`encode()`/`decode()`/`fragment_count()`/`make_fragment()` em
`btp/codec.hpp`/`btp/fragmentation.hpp`. Um relatório HID Full-Speed tem 64
octetos, com 1 octeto reservado ao Report ID: isso é significativamente menor
que os 210 octetos de payload do ESP-NOW, exigindo uma constante de limite
própria em vez de reaproveitar um perfil existente. Além disso, um relatório
HID de tamanho fixo sempre transmite os 63 octetos completos -- uma escrita
menor é preenchida com zeros pela pilha USB antes do envio -- então o
receptor não tem, por si só, como distinguir dado real de padding.

## Decisão

- Novo perfil `TransportProfile::UsbHid`: cada relatório HID contém no máximo
  um frame BTP completo (ou um fragmento) de até 62 octetos, sem COBS nem
  delimitador dentro desses 62 -- o relatório mais um octeto de prefixo de
  tamanho (ver abaixo) já é a unidade de framing entregue pela pilha USB do
  host.
- O firmware usa `USBHIDVendor(report_size, prepend_size=true)`: o primeiro
  dos 63 octetos de dado do relatório passa a ser um prefixo de tamanho (0 a
  62) que diz quantos octetos seguintes são dado real, permitindo ao receptor
  descartar o padding de zeros à direita. Um cliente desktop (hidapi) precisa
  replicar o mesmo prefixo ao montar o relatório OUT.
- Limites do novo perfil: `BTP_USB_HID_MAX_FRAME_SIZE = 62`,
  `BTP_USB_HID_MAX_PAYLOAD_SIZE = 22` (64 menos 1 octeto de Report ID, menos 1
  octeto de prefixo de tamanho, menos os 40 octetos fixos de header e CRC do
  envelope BTP v1).
- Fragmentação e reassembly reaproveitam `btp::fragment_count`/
  `make_fragment`/`Reassembler` sem modificação de comportamento -- já eram
  parametrizados por `TransportProfile`; só ganham mais uma entrada na tabela
  de limites.
- A interface HID opera sempre em modo protocolado, sem o par
  console/protocolo nem a troca textual `ENTER`/`READY` que a serial usa: não
  há operador humano digitando em um endpoint HID vendor-defined. A interface
  fica pronta para receber `HELLO` assim que o host a abre.
- `TransportProfile` continua sendo uma escolha local de cada lado da
  conexão (qual link físico está em uso), nunca um campo no wire -- nenhuma
  negociação nova é necessária alem do `HELLO`/versão já existentes.
- O dispositivo composto expõe CDC (perfil `Serial`) e HID (perfil `UsbHid`)
  simultaneamente, cada um com sua própria sessão BTP independente
  (`source_id`/`boot_id`/estado de sessão não compartilhados entre as duas
  interfaces).

As regras normativas completas estão em
[`TRANSPORT_USB_HID.md`](../TRANSPORT_USB_HID.md).

## Consequências

- Mensagens de controle pequenas como `HELLO` precisam fragmentar em vários
  relatórios mesmo sendo, em termos absolutos, mensagens pequenas -- é uma
  característica normal deste perfil, não uma falha de negociação ou de
  implementação.
- Uma mensagem lógica grande gera mais fragmentos por transporte HID do que
  pelos outros dois perfis (até 255 fragmentos de 22 octetos cobrem 5610
  octetos de payload lógico), aumentando a chance relativa de perda parcial
  sob tráfego pesado -- mitigado pelas mesmas garantias de reassembly
  tolerante a perda/duplicação/fora-de-ordem já exigidas para ESP-NOW.
- O checque de sanidade de `Reassembler::push` contra `kSerialMaxPayloadSize`
  permanece válido sem alteração: por ser o maior limite entre os três
  perfis, ele continua sendo um teto superior correto mesmo com o novo perfil
  menor.
- Nenhuma mudança é necessária no wire format do envelope BTP v1 nem nos
  perfis `EspNow`/`Serial` já publicados -- ambos permanecem byte a byte
  idênticos.

## Alternativas consideradas

- **Reaproveitar o perfil `Serial` (COBS) sobre a interface HID:** rejeitado
  porque o relatório HID já é uma unidade de framing discreta entregue pela
  pilha USB; aplicar COBS por cima desperdiçaria parte dos já escassos 22
  octetos de payload útil com bytes de escape sem necessidade.
- **USB bulk/vendor-specific (WinUSB) em vez de HID:** rejeitado para este
  perfil porque exigiria driver/descriptor especial (WinUSB via Microsoft OS
  descriptors) no lugar do driver de classe HID genérico já presente em
  qualquer sistema operacional moderno -- o objetivo explícito era "sem
  instalar driver nenhum".
- **Relatórios HID maiores que 64 octetos (High-Speed):** rejeitado porque o
  periférico USB nativo do ESP32-S3 é Full-Speed (12 Mbit/s); anunciar um
  endpoint maior não é suportado pelo hardware alvo.
- **Multiplexar múltiplos Report IDs na mesma interface para simular payload
  maior:** rejeitado porque não aumenta o teto de uma única transferência de
  interrupt (ainda 64 octetos cada); só adicionaria complexidade sem ganho de
  capacidade.
- **Determinar o tamanho real do relatório inspecionando `payload_size` do
  header BTP em vez de um prefixo de tamanho dedicado:** rejeitado porque
  faria a camada de transporte (que deve mover bytes sem conhecer BTP,
  mesmo princípio já aplicado a `SerialMux`/`EspNowManager` no lado do
  dongle) depender da semântica do envelope só para descobrir onde o
  padding começa -- o prefixo de tamanho mantém a camada de transporte
  genuinamente agnóstica de protocolo, ao custo de 1 octeto por relatório.

## Impacto de versão

Extensão compatível e opcional (`docs/VERSIONING.md`): novo perfil de
transporte negociável, sem alterar bytes, interpretação ou garantias dos
perfis `EspNow`/`Serial` já publicados. Classificada como `MINOR` --
`v1.1.0`.
