# BTP v1: formato binário no wire

## 1. Convenções normativas

As palavras **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT** e **MAY** são
requisitos normativos. Um octeto tem 8 bits. Offsets e tamanhos são expressos
em octetos e começam em zero.

O BTP serializa cada campo explicitamente. Uma implementação **MUST NOT**
transmitir a representação em memória de uma `struct`, nem derivar tamanhos de
`sizeof`, `size_t`, alinhamento, ABI ou do tipo implícito de um `enum`.

Todos os inteiros de mais de um octeto, inclusive o CRC, **MUST** ser
serializados em little-endian. `magic` é uma sequência de quatro octetos e não
um inteiro.

## 2. Composição do frame

Um frame BTP v1 é a concatenação exata abaixo:

```text
+----------------------+--------------------------+-------------+
| header (36 octetos)  | payload (payload_size)   | CRC32 (4)   |
+----------------------+--------------------------+-------------+
offset 0               offset 36                  offset 36 + N
```

Em que `N = payload_size` e:

```text
frame_size = 36 + payload_size + 4
```

Não há alinhamento, terminador ou padding entre os campos. O payload é uma
sequência opaca e **MAY** conter qualquer octeto, incluindo `0x00`, `0x0A` e
`0x0D`.

## 3. Cabeçalho fixo

O cabeçalho v1 tem exatamente 36 octetos:

| Offset | Tamanho | Campo | Tipo no wire | Valor ou significado |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | `magic` | 4 octetos | `42 54 50 00` (`BTP\0`) |
| 4 | 1 | `version` | `uint8` | `0x01` |
| 5 | 1 | `type` | `uint8` | Tipo lógico da mensagem, seção 4 |
| 6 | 2 | `flags` | `uint16_le` | Máscara de flags, seção 5 |
| 8 | 2 | `header_size` | `uint16_le` | `36` (`0x0024`) |
| 10 | 2 | `payload_size` | `uint16_le` | Octetos deste fragmento, sem header ou CRC |
| 12 | 4 | `source_id` | `uint32_le` | Identidade estável e não nula da origem |
| 16 | 4 | `boot_id` | `uint32_le` | Identidade não nula da inicialização da origem |
| 20 | 4 | `sequence` | `uint32_le` | Sequência da mensagem lógica |
| 24 | 8 | `timestamp_us` | `uint64_le` | Instante de origem em microssegundos |
| 32 | 2 | `object_id` | `uint16_le` | Objeto no namespace definido por `type` |
| 34 | 1 | `fragment_index` | `uint8` | Índice do fragmento, começando em zero |
| 35 | 1 | `fragment_count` | `uint8` | Número total de fragmentos |

Representação dos offsets:

```text
octeto  0                                                    35
        +------+--+--+----+----+----+--------+--------+--------+
        |magic |ver|typ|flag|h_sz|p_sz| source | boot   | seq    |
        +------+--+--+----+----+----+--------+--------+--------+
        0      4  5  6    8   10   12       16       20       24
        +----------------+--------+--+--+
        | timestamp_us   |object  |ix|ct|
        +----------------+--------+--+--+
        24              32       34 35 36
```

Para `version == 1`, `header_size` **MUST** ser 36. Um encoder v1 **MUST**
emitir esse valor e um decoder v1 **MUST** rejeitar qualquer outro valor. O
campo existe para tornar uma futura mudança de layout detectável, não para
permitir extensões silenciosas do cabeçalho v1.

`payload_size` descreve somente o payload presente no frame atual. Em uma
mensagem fragmentada ele não descreve o tamanho lógico total.

## 4. Tipos

`type` ocupa exatamente um `uint8` e seleciona um namespace de `object_id`:

| Valor | Nome | Uso |
| ---: | --- | --- |
| `0x00` | `INVALID` | Reservado; **MUST NOT** aparecer no wire |
| `0x01` | `TELEMETRY` | Amostras e dados de tópicos |
| `0x02` | `LOG` | Eventos e diagnóstico |
| `0x03` | `COMMAND` | Requisições, resultados e ações |
| `0x04` | `TERMINAL` | Entrada e saída de terminal como bytes opacos |
| `0x05` | `CONTROL` | Sessão, descoberta, manifesto, assinatura e status |
| `0x06` a `0xFF` | — | Reservados |

Os formatos internos desses payloads são especificados pelos documentos dos
respectivos canais; eles não alteram o envelope desta página. `object_id` **MAY**
ser zero quando o formato do tipo declarar que não existe objeto associado.

Um endpoint ou gateway v1 que receba um tipo reservado ou desconhecido
**MUST** validar tamanho e CRC, depois rejeitar o frame. Ele **MUST NOT**
reinterpretá-lo como outro tipo nem encaminhá-lo como se fosse conhecido. Um
encoder v1 **MUST NOT** emitir um tipo não atribuído.

## 5. Flags e fragmentação

`flags` ocupa exatamente um `uint16_le`:

| Máscara | Nome | Significado |
| ---: | --- | --- |
| `0x0001` | `FRAGMENTED` | O frame é parte de uma mensagem lógica com dois ou mais fragmentos |
| `0xFFFE` | — | Bits reservados; **MUST** ser zero |

Um decoder v1 **MUST** rejeitar um frame com qualquer bit reservado igual a
um. Flags desconhecidas não são ignoradas, pois podem mudar a interpretação do
frame.

Para uma mensagem não fragmentada:

- `FRAGMENTED` **MUST** estar limpo;
- `fragment_index` **MUST** ser 0;
- `fragment_count` **MUST** ser 1.

Para uma mensagem fragmentada:

- `FRAGMENTED` **MUST** estar marcado;
- `fragment_count` **MUST** estar entre 2 e 255;
- `fragment_index` **MUST** estar no intervalo de 0 a
  `fragment_count - 1`.

Cada fragmento é um frame BTP completo e possui seu próprio `payload_size` e
CRC. Os fragmentos de uma mensagem lógica **MUST** ter os mesmos `version`,
`type`, `flags`, `source_id`, `boot_id`, `sequence`, `timestamp_us`,
`object_id` e `fragment_count`; apenas `fragment_index`, `payload_size`,
payload e CRC podem variar. O payload lógico é a concatenação dos payloads em
ordem crescente de `fragment_index`.

## 6. Identidade, sequência e tempo

### 6.1 Origem e boot

`source_id` **MUST** ser não zero, estável durante a vida do dispositivo e
único no domínio em que seus frames podem ser roteados juntos. Dois
dispositivos distintos **MUST NOT** usar o mesmo `source_id`. A forma de
provisionar essa identidade fica fora do wire format.

`boot_id` **MUST** ser não zero, ser escolhido novamente em cada boot e
permanecer constante durante aquele boot. A origem **MUST** impedir o reuso de
um `boot_id` enquanto frames de uma inicialização anterior ainda puderem ser
recebidos ou permanecer em cache. Um contador persistente é preferível; uma
origem que use aleatoriedade **SHOULD** persistir o último valor e impedir
repetição imediata.

### 6.2 Sequência

`sequence` identifica uma mensagem lógica, não um frame físico. A origem
**MUST** atribuir um valor ainda não usado no mesmo par (`source_id`,
`boot_id`) e incrementá-lo uma vez por mensagem lógica. Todos os fragmentos da
mensagem **MUST** compartilhar a mesma sequência. Retransmitir a mesma
mensagem **MUST** preservar a sequência; criar uma nova mensagem **MUST** usar
outra.

A identidade canônica de uma mensagem lógica é (`source_id`, `boot_id`,
`sequence`). A origem **MUST NOT** deixar `sequence` dar a volta e reutilizar
um valor no mesmo boot; deve encerrar a emissão antes de esgotar os 32 bits.
Durante reassembly, divergência de `type`, `object_id`, `timestamp_us`, flags ou
contagem para a mesma identidade **MUST** invalidar a mensagem inteira.

Essas regras, junto da unicidade de `source_id`, impedem que fragmentos de
dispositivos diferentes compartilhem identidade.

### 6.3 Timestamp

`timestamp_us` é o número de microssegundos decorridos no relógio monotônico da
origem desde o início do boot identificado por `boot_id`. Para dados medidos,
ele **SHOULD** representar o instante de aquisição; para outros tipos, o
instante em que a mensagem lógica foi criada. Todos os fragmentos **MUST**
preservar o mesmo valor.

Um gateway, incluindo o dongle, **MUST NOT** substituir `timestamp_us` pela
hora de chegada, retransmissão ou encaminhamento. A correlação desse relógio
monotônico com tempo civil, se necessária, pertence ao protocolo de sessão e
não modifica o campo original.

## 7. CRC32

O CRC usa **CRC-32/ISO-HDLC** (também conhecido como CRC-32/IEEE):

| Parâmetro | Valor |
| --- | --- |
| Largura | 32 bits |
| Polinômio normal | `0x04C11DB7` |
| Polinômio refletido | `0xEDB88320` |
| Valor inicial | `0xFFFFFFFF` |
| Entrada refletida (`RefIn`) | `true` |
| Saída refletida (`RefOut`) | `true` |
| XOR final | `0xFFFFFFFF` |
| Check para ASCII `123456789` | `0xCBF43926` |

O acumulador cobre, nessa ordem, todos os octetos desde o primeiro octeto de
`magic` no offset 0 até o último octeto do payload. Portanto, cobre exatamente
`36 + payload_size` octetos. O próprio CRC não entra no cálculo.

O valor resultante **MUST** ser escrito como `uint32_le` imediatamente após o
payload, no offset `36 + payload_size`. Mesmo com payload vazio, o CRC cobre os
36 octetos do cabeçalho. Um frame com CRC divergente **MUST** ser rejeitado
antes de roteamento ou entrega ao consumidor. CRC detecta corrupção acidental;
ele não autentica a origem nem protege contra alteração intencional.

## 8. Limites normativos por transporte

As constantes abaixo fazem parte do contrato v1:

| Constante | Valor |
| --- | ---: |
| `BTP_V1_HEADER_SIZE` | 36 |
| `BTP_V1_CRC_SIZE` | 4 |
| `BTP_ESPNOW_MAX_FRAME_SIZE` | 250 |
| `BTP_ESPNOW_MAX_PAYLOAD_SIZE` | 210 |
| `BTP_SERIAL_MAX_FRAME_SIZE` | 4096 |
| `BTP_SERIAL_MAX_PAYLOAD_SIZE` | 4056 |

Um frame destinado a ESP-NOW **MUST** satisfazer `frame_size <= 250` e
`payload_size <= 210`. Cada datagrama contém somente os 36 octetos do header,
os `payload_size` octetos válidos e os 4 octetos do CRC. O emissor **MUST NOT**
completar o datagrama com zeros ou transmitir uma área fixa de 250 octetos.
Assim, o tamanho do datagrama **MUST** ser exatamente `40 + payload_size`.

Um frame BTP decodificado destinado à serial protocolada **MUST** satisfazer
`frame_size <= 4096` e `payload_size <= 4056`, antes da codificação de framing
da serial. A codificação COBS e seus delimitadores são definidos na
especificação de transporte e não contam em `frame_size`.

Uma mensagem lógica maior que o payload permitido pelo transporte **MUST** ser
fragmentada. Um encoder **MUST** verificar o limite do transporte antes de
escrever ou enviar. Um decoder **MUST** rejeitar o frame se o tamanho declarado
exceder o limite do transporte pelo qual ele foi recebido.

## 9. Validação de um frame

Sem ler além do buffer fornecido, um decoder v1 **MUST**:

1. exigir ao menos 40 octetos;
2. comparar `magic` com `42 54 50 00`;
3. exigir `version == 1` e `header_size == 36`;
4. calcular `frame_size = 40 + payload_size` com aritmética que detecte
   overflow e exigir igualdade exata com o tamanho recebido;
5. aplicar o limite do transporte, sem aceitar bytes extras ou padding;
6. verificar o CRC sobre header e payload;
7. validar tipo, flags, IDs e invariantes de fragmentação;
8. somente então rotear ou entregar o payload.

Versão, tipo, flags ou tamanho desconhecidos/inválidos causam rejeição
explícita. Um receptor **SHOULD** contabilizar o motivo da rejeição para
diagnóstico, sem tentar fallback para outro protocolo.

## 10. Exemplos hexadecimais

Os exemplos desta seção demonstram a validade do envelope. Documentos de canal
posteriores **MAY** impor requisitos adicionais ao conteúdo do payload.

### 10.1 LOG sem payload

Campos principais: `type=LOG`, `source_id=0x11223344`,
`boot_id=0xA1B2C3D4`, `sequence=1`, `timestamp_us=1000000`, `object_id=2`,
sem fragmentação e sem payload.

```text
42 54 50 00  01 03  00 00  24 00  00 00
44 33 22 11  d4 c3 b2 a1  01 00 00 00
40 42 0f 00 00 00 00 00  02 00  00 01
eb fd 00 d1
```

O CRC numérico é `0xD100FDEB`; no wire little-endian aparece como
`eb fd 00 d1`. O frame possui 40 octetos.

### 10.2 Segundo fragmento de TELEMETRY

Campos principais: `type=TELEMETRY`, `FRAGMENTED`,
`source_id=0x01020304`, `boot_id=0x10203040`,
`sequence=0x05060708`, `timestamp_us=0x0102030405060708`,
`object_id=0x1234`, fragmento 1 de 2 e payload `00 0a 0d ff`.

```text
42 54 50 00  01 01  01 00  24 00  04 00
04 03 02 01  40 30 20 10  08 07 06 05
08 07 06 05 04 03 02 01  34 12  01 02
00 0a 0d ff
2d 80 1f 40
```

O CRC numérico é `0x401F802D`; no wire aparece como `2d 80 1f 40`. O
payload demonstra que zero, LF, CR e `0xFF` são dados comuns e não
delimitadores.
