# Convenções e glossário

Este capítulo reúne o vocabulário que os documentos normativos usam sem
redefinir, e serve de referência de consulta durante a leitura dos demais.
Ele não cria requisito novo: a fonte normativa das convenções de serialização
é a seção 1 de [`BTP_V1.md`](BTP_V1.md), e cada capítulo de transporte define
os termos específicos do seu enlace.

## Palavras normativas

**MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT** e **MAY** são requisitos
normativos, sempre em inglês e em negrito, mesmo no meio de um texto em
português — a intenção é que a força de cada exigência seja visível na
varredura do texto e não dependa de interpretação de tradução.

| Palavra | Leitura |
| --- | --- |
| **MUST** / **MUST NOT** | Obrigação absoluta. Uma implementação que viola isso não é conforme, e os vetores de conformidade cobrem o caso. |
| **SHOULD** / **SHOULD NOT** | Recomendação forte. Desviar exige razão concreta e documentada; não torna a implementação não conforme. |
| **MAY** | Permissão. As duas escolhas são conformes, e nenhum peer pode assumir uma delas. |

Uma frase escrita em tom descritivo ("o gateway roteia", "o payload cresce 16
octetos") não é requisito: descreve consequência do que as regras já exigem
em outro lugar.

## Octetos, larguras e ordem de bytes

Um **octeto** tem 8 bits. Offsets e tamanhos são expressos em octetos e
começam em zero.

Larguras aparecem no formato `tipoNN_ordem`:

| Notação | Significado |
| --- | --- |
| `uint8` | Inteiro sem sinal de um octeto; ordem de bytes não se aplica. |
| `uint16_le`, `uint32_le`, `uint64_le` | Inteiro sem sinal, little-endian. |
| `float32_le`, `float64_le` | IEEE-754 de 4 ou 8 octetos, little-endian. |
| `utf8_u16` | `size:uint16_le` seguido de exatamente `size` octetos UTF-8, sem BOM e sem terminador. |
| `bytes_u32` | `size:uint32_le` seguido de exatamente `size` octetos arbitrários. |

Todo inteiro de mais de um octeto, inclusive o CRC, é serializado em
little-endian. `magic` é a exceção que não é inteiro: são quatro octetos em
ordem fixa (`42 54 50 00`).

O símbolo de concatenação indica octetos de um valor imediatamente seguidos
pelos de outro, sem separador, padding ou alinhamento entre as partes —
`ciphertext ‖ tag` é exatamente isso. Valores em hexadecimal usam o prefixo
`0x` no texto corrido e pares de dígitos separados por espaço em despejos de
octetos.

## Nada de representação de memória

O BTP serializa cada campo explicitamente. Uma implementação **MUST NOT**
transmitir a representação em memória de uma `struct`, nem derivar tamanho de
`sizeof`, `size_t`, alinhamento, ABI ou do tipo implícito de um `enum`.
Padding, ordem de campos e largura de tipos variam entre compiladores e
plataformas; nenhum deles faz parte do contrato.

Pela mesma razão, o payload é sempre uma sequência de octetos com tamanho
explícito. Não é string terminada em `0x00` nem delimitada por CR/LF:
`0x00`, `0x0A` e `0x0D` são dados válidos em qualquer posição.

## Rejeição explícita em vez de tolerância

Três regras atravessam todos os capítulos e explicam a maior parte das
decisões de validação:

- **Campo reservado é zero.** Todo campo ou bit reservado **MUST** ser zero
  ao emitir e **MUST** causar rejeição do frame quando recebido com outro
  valor. Um valor não atribuído nunca é ignorado.
- **Não existe modo legado.** Não há fallback, autodetecção nem parser
  alternativo para formato anterior. Um peer incompatível é rejeitado de
  forma explícita ([ADR 0001](decisions/0001-fonte-canonica-e-sem-legado.md)).
- **Descarte silencioso, sem NACK.** Frame com CRC divergente, tag AEAD
  inválido ou invariante violada é descartado antes de roteamento ou entrega,
  sem notificação ativa ao emissor
  ([ADR 0005](decisions/0005-transportes-entrega-e-integridade.md)).

## Os três níveis de entrega

Os capítulos de transporte distinguem três eventos que **não** são
equivalentes, e confundi-los é a origem mais comum de bug de integração:

| Termo | Significado |
| --- | --- |
| **Aceito** | A API local aceitou a tentativa de envio (`esp_now_send`, `SendReport`, `hid_write`). Diz respeito à fila local, não ao enlace. |
| **Confirmado no enlace** | O callback de envio reportou sucesso, ou a transferência USB completou sem erro de transporte. O octeto saiu; ninguém garante que foi compreendido. |
| **Concluído** | O protocolo de aplicação produziu a resposta esperada — um `COMMAND_RESULT`, um `HELLO_RESULT`. É o único que fala sobre semântica. |

## Glossário

Octeto
:   Oito bits. Unidade de todos os offsets e tamanhos deste protocolo.

Frame
:   `header (36) ‖ payload ‖ CRC32 (4)`. A unidade que atravessa um transporte
    e a extensão que o CRC cobre. Um fragmento já é um frame completo e
    independente.

Envelope
:   O conjunto de campos do header que identifica e roteia uma mensagem, em
    oposição ao payload que ele carrega. Usado quando o assunto é a semântica
    dos campos, não o layout deles.

Payload lógico
:   O conteúdo completo de uma mensagem, depois de remontar todos os
    fragmentos. É sobre ele que os capítulos de canal definem layout — e,
    quando `ENCRYPTED` está marcado, é ele que vale `ciphertext ‖ tag`.

Mensagem lógica
:   A unidade identificada pela tripla (`source_id`, `boot_id`, `sequence`).
    Todos os fragmentos de uma mensagem compartilham essa tripla, além de
    `type`, `flags`, `timestamp_us` e `object_id`.

Fragmento
:   Um frame que carrega parte de uma mensagem lógica, posicionado por
    `fragment_index` dentro de `fragment_count`. Máximo de 255 fragmentos por
    mensagem.

Canal
:   O valor de `type`: `TELEMETRY`, `LOG`, `COMMAND`, `TERMINAL` ou
    `CONTROL`. Canais nunca compartilham semântica, mesmo trafegando no mesmo
    enlace físico.

Fonte (`source_id`)
:   Identidade estável e não nula de quem originou a mensagem, única no
    domínio de roteamento. Um gateway retransmite sem substituí-la.

`boot_id`
:   Identidade da inicialização corrente da fonte, escolhida de novo a cada
    boot e nunca reusada enquanto frames do boot anterior possam existir. É o
    que impede que `sequence`, reiniciado em um reboot, colida com valores já
    usados.

`sequence`
:   Identifica a mensagem lógica dentro de um boot; **MUST NOT** se repetir
    nesse boot. Não é contador de frame, e em `COMMAND` funciona também como
    `request_id`.

`timestamp_us`
:   Instante monotônico, em microssegundos, criado por quem gerou o dado.
    Nenhum intermediário o substitui — é por ele que a apresentação plota.

`object_id`
:   Objeto dentro do namespace definido pelo `type`. Em `TELEMETRY` é o
    `topic_id`; em `COMMAND`/`CONTROL`/`TERMINAL`, o objeto daquele canal.

Tópico
:   O par (`source_id`, `topic_id`). `topic_id` é local ao namespace de cada
    fonte: fontes distintas podem usar o mesmo número para tópicos
    diferentes.

Schema
:   A tripla (`source_id`, `topic_id`, `schema_version`), que determina como
    decodificar o corpo de uma amostra. Nomes, tipos e unidades de campo são
    anunciados fora da amostra, nunca repetidos em cada mensagem.

Gateway
:   Papel que encaminha mensagens entre dois enlaces. Valida envelopes,
    encaminha canais e reenquadra entre perfis — mas não converte payload
    binário em texto nem redefine identidade, tempo ou schema. Um caminho BTP
    pode não ter gateway nenhum, ou ter mais de um.

Perfil de transporte
:   `EspNow`, `Serial` ou `UsbHid`. Muda o framing do enlace e os limites de
    tamanho; não muda o envelope, o CRC nem a semântica de nenhum campo.

Modo protocolado / console humano
:   Os dois estados da serial. No modo protocolado a porta transporta frames
    BTP em COBS; no console humano, texto para pessoas. A alternância é um
    handshake textual e a posse da porta é exclusiva.

COBS
:   *Consistent Overhead Byte Stuffing*. Codifica o frame inteiro de modo que
    o **bloco codificado** não contenha `0x00`, o que permite usar `0x00`
    como delimitador de pacote na serial. É framing de stream, não parte do
    envelope: não entra no `payload_size`, no `frame_size` nem no CRC.

CRC-32
:   CRC-32/ISO-HDLC calculado do primeiro octeto de `magic` até o último
    octeto do payload, gravado em seguida em little-endian. Detecta corrupção
    acidental; **não** autentica origem nem protege contra alteração
    intencional.

AEAD
:   *Authenticated Encryption with Associated Data*. Cifra o payload e, na
    mesma operação, autentica tanto o texto cifrado quanto dados que
    permanecem em claro (o AAD). Ver [Criptografia](CRYPTO.md).

Nonce
:   Valor de 96 bits, único por mensagem sob uma mesma chave, exigido pelas
    duas cifras. No BTP é `source_id ‖ boot_id ‖ sequence` — público por
    construção, o que é aceitável: nonce não é segredo, chave é.

AAD
:   Os 36 octetos do header, que seguem em claro no wire mas entram
    autenticados na operação AEAD: alterar um bit deles invalida o tag do
    mesmo jeito que alterar o ciphertext.

Tag
:   Os 16 octetos de autenticação produzidos pela cifra, calculados e
    verificados no nível da mensagem lógica — nunca por fragmento.

`CIPHER_ID`
:   Sub-campo de 2 bits em `flags` que registra no wire qual cifra produziu o
    payload (`0` AES-128-GCM, `1` ChaCha20-Poly1305). Identificação, não
    negociação.

Manifesto
:   O catálogo de fontes, tópicos e ações que o gateway expõe pelo canal
    `CONTROL` para descoberta, e de onde a apresentação tira schema e
    parâmetros.

Vetor de conformidade
:   Par `.json`/`.bin` em `test-vectors/`: a descrição legível e o frame cru
    correspondente. É o critério objetivo de conformidade — uma implementação
    deve produzir e consumir exatamente aqueles octetos.

ADR
:   *Architecture Decision Record*. Registro imutável de uma decisão, com
    contexto, alternativas e consequências. Quando uma decisão muda, um
    registro novo a substitui; o histórico não é reescrito.
