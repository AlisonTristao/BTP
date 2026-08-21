# BTP v1: telemetria, arrays e schemas

## 1. Convenções normativas

As convenções normativas — palavras **MUST**/**SHOULD**/**MAY**, octetos e a
notação de largura e ordem de bytes, incluindo os valores de ponto flutuante
usados neste documento — estão em [`CONVENTIONS.md`](CONVENTIONS.md) e valem
integralmente aqui.

Este documento especifica o payload lógico de mensagens cujo `type` no
envelope BTP v1 é `TELEMETRY`. Envelope, CRC, limites físicos e identificação
de fragmentos continuam regidos por [`BTP_V1.md`](BTP_V1.md).

## 2. Identidade de tópico e schema

Em uma mensagem `TELEMETRY`, o campo `object_id` do envelope **MUST** ser
interpretado como `topic_id` e **MUST** ser diferente de zero. Um tópico é
identificado pelo par:

```text
(source_id, topic_id)
```

`topic_id` é local ao namespace de uma fonte. Fontes distintas **MAY** usar o
mesmo valor numérico para tópicos diferentes. O par deve permanecer estável
entre boots enquanto conservar a mesma finalidade semântica.

O schema exato usado para decodificar uma amostra é identificado pela tripla:

```text
(source_id, topic_id, schema_version)
```

`schema_version` é um `uint16_le` entre 1 e 65535. O valor zero é inválido. A
versão é monotônica dentro de um tópico: uma origem **MUST NOT** reutilizar uma
versão para outro layout ou outra interpretação. Depois de usar 65535, uma
fonte que precisar de outro schema **MUST** alocar um novo `topic_id`; a versão
não pode dar a volta.

Todo payload lógico `TELEMETRY` possui a seguinte forma, independentemente do
encoding:

| Offset | Tamanho | Campo | Tipo no wire |
| ---: | ---: | --- | --- |
| 0 | 2 | `schema_version` | `uint16_le` |
| 2 | variável | `encoded_body` | bytes segundo o encoding declarado pelo schema |

`schema_version` seleciona um schema anunciado fora das amostras. A definição
completa do schema — nomes, tipos, unidades e demais metadados — **MUST NOT**
ser repetida em cada amostra. O mecanismo binário de anúncio pertence ao
[manifesto e à descoberta](COMMANDS_AND_ACTIONS.md); este documento define o
conteúdo que esse mecanismo representa.

Em uma mensagem fragmentada, essa estrutura pertence ao payload **lógico**:
os fragmentos carregam fatias consecutivas de seus bytes. A versão aparece
uma vez, no início do resultado do reassembly, e não é repetida em cada
fragmento físico.

## 3. Modelo de schema

Um schema de telemetria **MUST** declarar ao menos:

- `source_id`, `topic_id` e `schema_version`;
- um nome estável e legível do tópico;
- o `encoding` do corpo;
- para encodings estruturados, uma lista ordenada de campos.

Neste documento, `JSON_UTF8`, `CSV_UTF8`, `PACKED_LE` e `TLV_LE` são
encodings estruturados. `OPAQUE_BYTES` e `UTF8` **MAY** declarar um único
`field_id` lógico para permitir binding do valor integral, mas não usam o
modelo numérico abaixo.

Cada campo estruturado **MUST** declarar:

| Propriedade | Regra |
| --- | --- |
| `field_id` | `uint16` não zero, único, estável e nunca reutilizado dentro do tópico |
| `name` | nome legível, não usado como identidade no wire |
| `order` | índice único, contíguo e começando em zero |
| `type` | um dos tipos definidos na seção 5 |
| `unit` | símbolo estável da unidade de engenharia; `1` significa adimensional |
| `scale` | multiplicador finito; padrão `1` |
| `offset` | deslocamento finito; padrão `0` |
| `element_count` | `1`, uma quantidade fixa entre 1 e 65535, ou `variable` |
| `max_element_count` | obrigatório, entre 1 e 65535, quando a quantidade é variável |
| `nullable` | booleano; padrão `false` |

A conversão de um valor numérico bruto `raw` para o valor de engenharia é:

```text
engineering_value = raw * scale + offset
```

`scale` e `offset` são metadados do schema, não bytes da amostra. Eles **MUST
NOT** ser aplicados a `bool` e são opcionais para enums; quando usados em enum,
não alteram a seleção do rótulo, que sempre usa o valor inteiro bruto.

Um campo escalar usa `element_count = 1`. Em um array, `nullable` se refere ao
campo inteiro, não a elementos individuais. Um schema v1 **MUST NOT** declarar
elementos nullable dentro de um array.

Alterações de tipo, ordem, quantidade, unidade, escala, offset, nulabilidade,
encoding ou significado exigem um novo `schema_version`. Mudar apenas rótulos
de apresentação que não alteram a identidade nem a interpretação dos dados
não exige mudar os bytes da amostra.

## 4. Encodings

O schema atribui um dos seguintes códigos `uint8`; o código não é repetido na
amostra porque é obtido pelo `schema_version`:

| Valor | Nome | Uso |
| ---: | --- | --- |
| `0x00` | `INVALID` | reservado; **MUST NOT** ser anunciado |
| `0x01` | `OPAQUE_BYTES` | bytes cujo significado é externo ao BTP |
| `0x02` | `UTF8` | texto UTF-8 com tamanho explícito |
| `0x03` | `JSON_UTF8` | um valor JSON codificado em UTF-8 |
| `0x04` | `CSV_UTF8` | um registro CSV codificado em UTF-8 |
| `0x05` | `PACKED_LE` | campos compactos em ordem de schema |
| `0x06` | `TLV_LE` | campos identificados por tag-comprimento-valor |
| `0x07` a `0xFF` | — | reservados |

`PACKED_LE` é o encoding padrão de produção. `CSV_UTF8` **SHOULD** ficar
restrito a teste e diagnóstico. Um produtor **MUST NOT** trocar o encoding sem
publicar uma nova versão de schema.

### 4.1 `OPAQUE_BYTES`

O corpo inteiro é uma sequência opaca, delimitada somente por seu tamanho. Ele
**MAY** conter qualquer octeto, inclusive `0x00`, CR e LF. O BTP não expõe
campos internos desse corpo; consumidores só podem vinculá-lo pelo `field_id`
lógico do valor integral, se declarado. `nullable` e transformação por escala
não são admitidos.

### 4.2 `UTF8`

O corpo inteiro é uma sequência UTF-8 bem formada, sem BOM e sem terminador
implícito. `0x00` representa o caractere U+0000 e não termina a amostra. O
schema expõe no máximo um campo textual integral, identificado pelo
`field_id` lógico se declarado; `nullable`, arrays, escala e offset não são
admitidos. UTF-8 inválido invalida a amostra.

### 4.3 `JSON_UTF8`

O corpo, após decodificação UTF-8, **MUST** conter exatamente um array JSON de
nível superior, sem BOM ou dados não brancos posteriores. O array contém uma
entrada por campo, em ordem de schema; arrays de campo são arrays JSON
aninhados e devem respeitar a quantidade fixa ou máxima declarada. Um campo
nullable ausente usa o valor JSON `null`; `null` em campo não nullable é erro.
Inteiros e enums são números JSON inteiros, floats são números JSON, e bool é
o valor JSON `true` ou `false`. Todos ainda devem respeitar largura, contagem e
as políticas da seção 6.

### 4.4 `CSV_UTF8`

O corpo contém exatamente um registro UTF-8, sem cabeçalho e sem terminador de
linha. Campos seguem a ordem do schema, são separados por vírgula (`0x2C`) e
podem ser cercados por aspas duplas; uma aspa dentro de campo com aspas é
duplicada. CR ou LF fora de aspas são inválidos. `CSV_UTF8` v1 admite somente
campos escalares. Inteiros e enums usam dígitos decimais, com sinal `-`
somente para tipos signed; floats usam a gramática decimal de números JSON; e
bool usa `true` ou `false`. O literal sem aspas `null` representa campo
nullable ausente. Campo vazio é inválido.

Após a análise lexical, o valor **MUST** caber no tipo do schema e obedecer às
regras da seção 6. CSV não define o wire canônico de produção e **MUST NOT**
ser usado como fallback quando um encoding binário falhar.

### 4.5 `PACKED_LE`

O corpo começa com um bitmap de presença apenas se houver campos nullable. Se
`K` é a quantidade de campos nullable, o bitmap tem `ceil(K / 8)` octetos. O
bit zero do primeiro octeto corresponde ao primeiro campo nullable na ordem do
schema; bits avançam do menos para o mais significativo e depois ao próximo
octeto. Bit `1` significa presente e bit `0`, nulo. Bits não atribuídos no
último octeto **MUST** ser zero.

Depois do bitmap, campos presentes aparecem uma única vez em `order`
crescente, sem padding ou alinhamento. Campos não nullable aparecem sempre.
Um campo nullable ausente não ocupa bytes além de seu bit de presença.

- Um escalar ocupa exatamente a largura de seu tipo.
- Um array fixo contém exatamente `element_count` elementos consecutivos e
  não carrega contagem.
- Um array variável começa com `element_count:uint16_le`, seguido dessa
  quantidade de elementos consecutivos.
- A contagem de um array variável **MUST** ser menor ou igual a
  `max_element_count` do schema.

O decoder conhece os limites pelo schema e **MUST** consumir exatamente o
corpo. Truncamento, contagem excessiva ou bytes restantes invalidam a amostra.

### 4.6 `TLV_LE`

O corpo é a concatenação de zero ou mais entradas:

```text
+----------------------+----------------------+----------------------+
| field_id:uint16_le   | value_size:uint16_le | value[value_size]    |
+----------------------+----------------------+----------------------+
0                      2                      4
```

Como `value_size` é `uint16_le`, cada valor individual **MUST** ocupar no
máximo 65535 octetos. Um schema `TLV_LE` cujo valor máximo calculado exceda
esse limite é inválido.

Entradas **MUST** estar em `field_id` crescente e um ID **MUST NOT** se
repetir. O valor usa o tipo e as regras de array do schema; arrays variáveis
mantêm o prefixo `element_count:uint16_le`. `value_size` deve ser exatamente o
tamanho calculado do valor.

Um campo nullable ausente é representado pela ausência de sua entrada. Todo
campo não nullable **MUST** estar presente. Um ID desconhecido deve ter seu
`value_size` validado para que o parser avance com segurança, mas torna a
amostra incompatível e **MUST** causar sua rejeição e contabilização. Uma
entrada truncada, duplicada, fora de ordem ou com tamanho incompatível também
invalida a amostra.

`TLV_LE` permite extensões esparsas, mas custa quatro octetos por campo. Ele
não substitui a troca explícita de versão quando a interpretação de um campo
existente muda.

## 5. Tipos no wire

Os tipos permitidos em campos estruturados são:

| Tipo | Tamanho por elemento | Representação |
| --- | ---: | --- |
| `uint8` | 1 | inteiro sem sinal |
| `uint16` | 2 | inteiro sem sinal, little-endian |
| `uint32` | 4 | inteiro sem sinal, little-endian |
| `uint64` | 8 | inteiro sem sinal, little-endian |
| `int8` | 1 | complemento de dois |
| `int16` | 2 | complemento de dois, little-endian |
| `int32` | 4 | complemento de dois, little-endian |
| `int64` | 8 | complemento de dois, little-endian |
| `float32` | 4 | IEEE-754 binary32 pelos bits, little-endian |
| `float64` | 8 | IEEE-754 binary64 pelos bits, little-endian |
| `bool` | 1 | `0x00` falso, `0x01` verdadeiro |
| `enum8` | 1 | valor inteiro sem sinal e tabela no schema |
| `enum16` | 2 | valor inteiro sem sinal, little-endian, e tabela no schema |

Valores signed **MUST** usar complemento de dois na largura declarada.
Floats **MUST** ser convertidos para seu padrão de bits IEEE-754 e esses bits,
não uma representação textual ou cast de memória, são serializados em
little-endian. Um `bool` diferente de `0x00` e `0x01` é inválido.

## 6. Política de valores e erros

### 6.1 Valores não finitos

NaN positivo ou negativo, infinito positivo ou negativo e qualquer resultado
não finito são inválidos em telemetria BTP v1. Um produtor **MUST NOT**
emiti-los. Um receptor **MUST** rejeitar a amostra lógica que os contenha e
reportar o motivo. Ausência de medição deve usar um campo nullable, não NaN.

### 6.2 Enum desconhecido

Um valor de enum ausente da tabela do schema não muda offsets nem torna o
payload estruturalmente ambíguo. O receptor **MUST** preservar o inteiro bruto,
marcar o campo como enum desconhecido e **MUST NOT** atribuir-lhe outro rótulo.
Ele **SHOULD** reportar a ocorrência. Os demais campos da amostra continuam
válidos.

### 6.3 Payload curto, longo ou inconsistente

O receptor **MUST** validar limites antes de cada leitura e nunca ler além do
buffer reassemblado. Payload com menos de dois octetos, campo truncado, array
com contagem excessiva, bitmap inválido, tamanho TLV divergente ou bytes não
consumidos **MUST** ser rejeitado por inteiro. Não existe preenchimento,
terminador implícito, valor padrão para bytes ausentes nem decodificação
parcial de uma amostra estruturalmente inválida.

### 6.4 Schema incompatível ou ausente

Antes de interpretar o corpo, o consumidor **MUST** possuir exatamente a
tripla (`source_id`, `topic_id`, `schema_version`) anunciada pela fonte. Se a
versão é zero, desconhecida, incompatível com o encoding implementado ou
diverge do schema anunciado, o consumidor **MUST**:

1. rejeitar a amostra lógica sem tentar outra versão, encoding ou heurística;
2. não entregar valores parciais a gráficos ou automações;
3. registrar ao menos `source_id`, `topic_id`, versão recebida e motivo;
4. incrementar um contador observável de rejeições;
5. **SHOULD** solicitar atualização do catálogo quando o protocolo de
   descoberta estiver disponível.

Essa rejeição é do payload de telemetria; ela não transforma um frame cujo
envelope e CRC são válidos em erro de transporte.

## 7. Fragmentação e publicação

Uma amostra frequente **SHOULD** caber em um único frame do transporte mais
restritivo de seu caminho. Em ESP-NOW isso significa no máximo 210 octetos de
payload BTP, dos quais dois são `schema_version`, restando até 208 octetos para
o corpo codificado.

Quando a amostra exceder o limite, ela usa a fragmentação de `BTP_V1.md`. Um
consumidor **MUST NOT** decodificar, publicar, plotar ou entregar fragmentos
isolados. A amostra só pode ser publicada depois de reassembly completo,
validação de todos os frames e validação integral do payload lógico. Mensagem
incompleta, expirada ou inconsistente é descartada por inteiro.

## 8. Binding do cliente e apresentação

Um cliente identifica um dado escalar pelo binding:

```text
(source_id, topic_id, field_id)
```

Para um elemento de array, acrescenta-se o índice:

```text
(source_id, topic_id, field_id, element_index)
```

Se um `element_index` não existe na amostra atual de um array variável, esse
binding está temporariamente sem valor; o cliente **MUST NOT** reutilizar um
elemento anterior nem fabricar zero.

O `schema_version` seleciona como a amostra atual é decodificada, mas não
substitui a identidade estável do campo. Se uma nova versão remover ou mudar
incompativelmente um `field_id`, o binding deve ficar indisponível até ser
reconfigurado; ele não pode migrar por nome ou posição.

Um binding **MUST NOT** conter ID de gráfico, painel, cor, eixo ou posição de
tela. Vários gráficos podem consumir o mesmo campo. Rótulos, cores, limites de
eixo, layout, agregação e outras preferências visuais são configuração do
cliente e não fazem parte do payload de telemetria nem do schema de dados.

## 9. Schemas de exemplo

Os exemplos abaixo mostram o modelo lógico representado pelo
[manifesto](COMMANDS_AND_ACTIONS.md). Eles não repetem a serialização do
próprio manifesto.

### 9.1 Motor

```text
source_id:      0x11223344
topic_id:       0x0101
schema_version: 1
name:           motor_state
encoding:       PACKED_LE
```

| order | field_id | name | type | unit | scale | offset | element_count | nullable |
| ---: | ---: | --- | --- | --- | ---: | ---: | ---: | --- |
| 0 | 1 | `left_speed` | `float32` | `rad/s` | 1 | 0 | 1 | false |
| 1 | 2 | `right_speed` | `float32` | `rad/s` | 1 | 0 | 1 | false |
| 2 | 3 | `left_current` | `int16` | `A` | 0.01 | 0 | 1 | false |
| 3 | 4 | `right_current` | `int16` | `A` | 0.01 | 0 | 1 | false |

Para `schema_version=1`, velocidades `1.5` e `-2.25 rad/s` e correntes brutas
`300` e `-40`, o payload lógico é:

```text
01 00                                      schema_version = 1
00 00 c0 3f                               left_speed = 1.5f
00 00 10 c0                               right_speed = -2.25f
2c 01                                     left_current raw = 300 (3.00 A)
d8 ff                                     right_current raw = -40 (-0.40 A)
```

Forma contínua:

```text
01 00 00 00 c0 3f 00 00 10 c0 2c 01 d8 ff
```

O corpo contém exatamente 12 octetos de valores; com a versão, o payload tem
14. Não há nomes, separadores, terminador ou padding. Os octetos `0x00` dentro
dos floats são dados comuns.

### 9.2 IMU

```text
source_id:      0x11223344
topic_id:       0x0102
schema_version: 1
name:           imu
encoding:       PACKED_LE
```

| order | field_id | name | type | unit | scale | offset | element_count | nullable |
| ---: | ---: | --- | --- | --- | ---: | ---: | ---: | --- |
| 0 | 1 | `acceleration` | `float32` | `m/s^2` | 1 | 0 | 3 | false |
| 1 | 2 | `angular_velocity` | `float32` | `rad/s` | 1 | 0 | 3 | false |
| 2 | 3 | `temperature` | `int16` | `Cel` | 0.01 | 0 | 1 | true |

Há um campo nullable, portanto o corpo começa com bitmap de um octeto. Com
temperatura presente, o tamanho do corpo é `1 + 3*4 + 3*4 + 2 = 27` octetos;
ausente, é 25. Os sete bits não usados do bitmap permanecem zero.

### 9.3 Sensor de linha

```text
source_id:      0x11223344
topic_id:       0x0103
schema_version: 1
name:           line_sensor
encoding:       PACKED_LE
```

| order | field_id | name | type | unit | scale | offset | element_count | nullable |
| ---: | ---: | --- | --- | --- | ---: | ---: | --- | --- |
| 0 | 1 | `reflectance` | `uint16` | `%` | 0.01 | 0 | `variable` (máx. 32) | false |
| 1 | 2 | `centroid` | `int16` | `1` | 0.001 | 0 | 1 | true |
| 2 | 3 | `quality` | `enum8` | `1` | 1 | 0 | 1 | false |

Tabela de `quality`: `0=invalid`, `1=low`, `2=good`, `3=saturated`. Como
`centroid` é o único nullable, o primeiro octeto do corpo é seu bitmap. Em
seguida vêm `element_count:uint16_le`, os elementos de `reflectance`, o
`centroid` somente quando presente e `quality`.

### 9.4 Tópicos iniciais do firmware Bally

O firmware `bally_software` registra estaticamente estes schemas `PACKED_LE`
versão 1. Os IDs são locais à fonte do robô, mas permanecem estáveis entre
boots:

| topic_id | name | order / field_id / campo | tipo | unidade |
| ---: | --- | --- | --- | --- |
| `0x0001` | `protocol.test` | 0 / 1 / `counter` | `uint32` | `1` |
| `0x0001` | `protocol.test` | 1 / 2 / `value` | `float32` | `1` |
| `0x0002` | `robot.state` | 0 / 1 / `state` | `uint8` | `1` |

O payload canônico de `protocol.test` usa `counter=0x01020304` e um
`float32` finito com bits IEEE-754 `0x3F0D0A00`:

```text
01 00 04 03 02 01 00 0a 0d 3f
```

Assim, `0x00`, LF (`0x0A`) e CR (`0x0D`) aparecem dentro da amostra e não
possuem semântica de terminador. O frame completo correspondente está em
`test-vectors/v1/valid/protocol_test.bin`.

## 10. Ordem de validação

Depois de validar o envelope segundo `BTP_V1.md` e concluir eventual
reassembly, um consumidor **MUST**:

1. exigir ao menos dois octetos e ler `schema_version`;
2. resolver exatamente (`source_id`, `topic_id`, `schema_version`);
3. confirmar que suporta o encoding anunciado;
4. validar estrutura, comprimentos, contagens, presença e tipos sem acesso
   fora do buffer;
5. aplicar as políticas de bool, float e enum;
6. somente então converter escala/unidade e publicar a amostra.

Falha em qualquer passo descarta a amostra lógica inteira, salvo o caso
explicitamente recuperável de enum desconhecido da seção 6.2.
