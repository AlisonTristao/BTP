# O frame no wire

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

Esta seção é a fonte normativa das convenções; a notação de largura de campo,
os termos usados em todos os capítulos e o glossário do protocolo estão
reunidos em [`CONVENTIONS.md`](CONVENTIONS.md).

## 2. Composição do frame

Um frame wire v1 é a concatenação exata abaixo:

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

O cabeçalho do wire v1 tem exatamente 36 octetos:

| Offset | Tamanho | Campo | Tipo no wire | Valor ou significado |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | `magic` | 4 octetos | `42 54 50 00` (`BTP\0`) |
| 4 | 1 | `version` | `uint8` | `0x01` ou `0x02` (seção 8) |
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

Para `version == 1`, `header_size` **MUST** ser 36. Um encoder de wire v1 **MUST**
emitir esse valor e um decoder de wire v1 **MUST** rejeitar qualquer outro valor. O
campo existe para tornar uma futura mudança de layout detectável, não para
permitir extensões silenciosas do cabeçalho do wire v1.

Um encoder que implemente a criptografia AEAD do payload (seção 8) **MUST**
emitir `version == 0x02` em qualquer frame com `ENCRYPTED` marcado; **MAY**
continuar emitindo `version == 0x01` em qualquer frame com `ENCRYPTED`
limpo, preservando compatibilidade byte a byte com um decoder que só
implemente esta especificação sem a seção 8. `version == 0x02` com
`ENCRYPTED` limpo **MAY** ocorrer — declara apenas que o emissor implementa
a seção 8, sem exigi-la — mas não é obrigatório. O layout do cabeçalho, seus
36 octetos, offsets e o significado de cada campo são idênticos entre
`version == 1` e `version == 2`; `header_size` **MUST** permanecer 36 em
ambos os casos, pelas mesmas regras já descritas acima. Um decoder que não
implemente a seção 8 **MUST** tratar `version == 2` como valor desconhecido
e rejeitar o frame, exatamente como rejeitaria qualquer `version` diferente
de 1; um decoder que implemente a seção 8 **MUST** aceitar tanto
`version == 1` quanto `version == 2`.

`payload_size` descreve somente o payload presente no frame atual. Em uma
mensagem fragmentada ele não descreve o tamanho lógico total.

## 4. Tipos

`type` ocupa exatamente um `uint8` e seleciona um namespace de `object_id`:

| Valor | Nome | Uso |
| ---: | --- | --- |
| `0x00` | `INVALID` | Reservado; **MUST NOT** aparecer no wire |
| `0x01` | `TELEMETRY` | Amostras e dados de tópicos; payload em [`TELEMETRY.md`](TELEMETRY.md) |
| `0x02` | `LOG` | Eventos e diagnóstico |
| `0x03` | `COMMAND` | Requisições, resultados e ações; payload em [`COMMANDS_AND_ACTIONS.md`](COMMANDS_AND_ACTIONS.md) |
| `0x04` | `TERMINAL` | Entrada e saída de terminal como bytes opacos; payload no mesmo documento |
| `0x05` | `CONTROL` | Sessão, descoberta, manifesto, assinatura e status; payload no mesmo documento |
| `0x06` a `0xFF` | — | Reservados |

Os formatos internos desses payloads são especificados pelos documentos dos
respectivos canais; eles não alteram o envelope desta página. `object_id` **MAY**
ser zero quando o formato do tipo declarar que não existe objeto associado.

Um endpoint ou gateway de wire v1 que receba um tipo reservado ou desconhecido
**MUST** validar tamanho e CRC, depois rejeitar o frame. Ele **MUST NOT**
reinterpretá-lo como outro tipo nem encaminhá-lo como se fosse conhecido. Um
encoder de wire v1 **MUST NOT** emitir um tipo não atribuído.

## 5. Flags e fragmentação

`flags` ocupa exatamente um `uint16_le`:

| Máscara | Nome | Significado |
| ---: | --- | --- |
| `0x0001` | `FRAGMENTED` | O frame é parte de uma mensagem lógica com dois ou mais fragmentos |
| `0x0002` | `ENCRYPTED` | O payload lógico é `ciphertext ‖ tag` em vez de dado em claro; ver seção 8 |
| `0x000C` | `CIPHER_ID` | Sub-campo de 2 bits (bits 2 e 3, deslocamento 2) que identifica a cifra AEAD usada quando `ENCRYPTED` está marcado; ver seção 8.1 |
| `0xFFF0` | — | Bits reservados; **MUST** ser zero |

Diferente de `FRAGMENTED` e `ENCRYPTED`, `CIPHER_ID` não é uma flag booleana
isolada: é um sub-campo de 2 bits — um valor de 1-em-4, não duas flags
independentes — extraído de `flags` com a máscara `0x000C` e o deslocamento
2. Essa escolha evita que o wire consiga representar uma combinação
ambígua do tipo "duas cifras marcadas ao mesmo tempo"; a seção 8.1 define os
valores atribuídos e as regras de consistência com `ENCRYPTED`.

Um decoder de wire v1 **MUST** rejeitar um frame com qualquer bit reservado igual a
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

Um gateway **MUST NOT** substituir `timestamp_us` pela
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

## 8. Criptografia AEAD do payload

Quando o bit `ENCRYPTED` (`0x0002`, seção 5) está marcado, o payload lógico
descrito pelas seções anteriores passa a ser `ciphertext ‖ tag` em vez de
dado em claro. Esta seção define o algoritmo, o nonce, os dados associados
(AAD), o nível em que o tag é calculado e verificado, a chave, a estratégia
de negociação e as regras de migração associadas a essa extensão. Quando
`ENCRYPTED` está limpo, nada nesta seção se aplica e o frame segue
integralmente as regras das seções 1 a 7.

### 8.1 Algoritmo

Um endpoint **MUST** usar **AES-128-GCM** como algoritmo primário de
cifragem autenticada.

Um endpoint sem acelerador AES em hardware **SHOULD** usar
**ChaCha20-Poly1305** em vez de AES-128-GCM: mesmo tamanho de nonce (96
bits) e de tag (128 bits), constante em tempo por construção, sem depender
de aceleração de hardware para evitar side-channel de timing.

O sub-campo `CIPHER_ID`, dois bits do campo `flags` (bits 2 e 3, máscara
`0x000C`, deslocamento 2 — seção 5), sinaliza no wire qual das duas cifras
acima produziu o payload de um frame:

| `CIPHER_ID` | Cifra |
| ---: | --- |
| `0` | AES-128-GCM (cifra primária; valor padrão) |
| `1` | ChaCha20-Poly1305 |
| `2` ou `3` | Reservado para cifras futuras |

Quando `ENCRYPTED` está limpo, não há cifra em uso pelo frame: `CIPHER_ID`
**MUST** ser `0` nesse caso, e um decoder **MUST** rejeitar um frame com
`ENCRYPTED` limpo e `CIPHER_ID` diferente de zero. Quando `ENCRYPTED` está
marcado, `CIPHER_ID` **MUST** ser `0` ou `1` — os únicos valores atribuídos
por esta especificação —, e um decoder **MUST** rejeitar `CIPHER_ID == 2` ou
`CIPHER_ID == 3`, pelo mesmo princípio já aplicado a bits de flag reservados
(seção 5): um valor não reconhecido não é ignorado, é motivo de rejeição
explícita do frame.

`CIPHER_ID` identifica no wire qual cifra foi usada; ele não é, por si só,
negociação em tempo de execução nem dispensa configuração prévia. Dois
endpoints que troquem frames com `ENCRYPTED` marcado **MUST** continuar
configurados fora de banda para suportar e aceitar as cifras que pretendem
usar entre si (seção 8.5); esta especificação não define como um endpoint
decide aceitar ou rejeitar um `CIPHER_ID` atribuído (`0` ou `1`) que ele
mesmo não implementa — essa decisão permanece fora do escopo do wire format,
na mesma linha da estratégia de negociação descrita na seção 8.6.

### 8.2 Nonce

O nonce de 96 bits exigido por AES-128-GCM e por ChaCha20-Poly1305 **MUST**
ser construído como a concatenação, nessa ordem, de três campos que já
existem no cabeçalho (seção 3), sem nenhum octeto adicional:

```text
nonce = source_id (4 octetos) ‖ boot_id (4 octetos) ‖ sequence (4 octetos)
```

Nenhum desses campos muda de posição, tamanho ou codificação; o nonce é
derivado do cabeçalho, não escrito separadamente nele.

A unicidade do nonce decorre inteiramente de regras já normativas na seção
6: `source_id` **MUST** ser não nulo e único no domínio de roteamento
(seção 6.1); `sequence` **MUST NOT** se repetir dentro do mesmo boot (seção
6.2). Isoladamente, porém, isso não bastaria: sem `boot_id` no nonce, um
reboot reiniciaria `sequence` a partir de valores já usados, reutilizando
nonces sob a mesma chave em relação a sessões anteriores — o cenário
catastrófico para um modo GCM, que permite recuperar o keystream e forjar
tags. Como `boot_id` **MUST** ser escolhido de novo a cada boot e nunca
reusado enquanto frames de um boot anterior possam existir (seção 6.1), cada
boot ocupa uma faixa de nonce disjunta de qualquer boot anterior do mesmo
`source_id`; a combinação dos três campos é única por construção das regras
que o protocolo já exige, sem contador de nonce dedicado nem campo novo.

### 8.3 Dados associados (AAD)

Os 36 octetos do cabeçalho (seção 3) **MUST** ser usados como dados
associados (AAD) da operação AEAD. O cabeçalho permanece em claro no wire —
necessário para que o pipeline de validação e roteamento existente (seção 10)
opere por `type`, `object_id` e demais campos antes de decifrar — mas, por
estar autenticado como AAD, qualquer bit alterado nele invalida o tag
exatamente como alterar o payload invalidaria.

Como o tag é calculado uma única vez, sobre a mensagem lógica e antes de
fragmentar (seção 8.4), o cabeçalho usado como AAD é o da **mensagem
lógica**, e não o de um fragmento. Ele **MUST** ser serializado exatamente
como a seção 3 define, com estes três campos canonicalizados:

| Campo | Valor no AAD |
| --- | --- |
| `payload_size` | tamanho do payload lógico cifrado completo, isto é de `ciphertext ‖ tag` |
| `flags` | os flags do frame com o bit `FRAGMENTED` (`0x0001`) **limpo** |
| `fragment_index` / `fragment_count` | `0` e `1`, respectivamente |

Todos os demais campos **MUST** aparecer no AAD com o mesmo valor que
carregam no wire, inclusive `version`, que vale `0x02` (seção 8.1).

Esses três são exatamente os campos que variam entre fragmentos de uma mesma
mensagem, e nenhum outro campo do cabeçalho varia dentro de uma mensagem
lógica (seção 6.2). Um receptor, portanto, **MUST** conseguir reconstruir o
AAD a partir do cabeçalho de qualquer fragmento, e um emissor **MUST NOT**
fazer `fragment_index`, `fragment_count` ou o bit `FRAGMENTED` participarem
do AAD.

A consequência é deliberada: o tag não depende de como a mensagem foi
fragmentada. Um gateway **MAY** reassemblar uma mensagem cifrada e
refragmentá-la para um perfil de transporte de limite diferente (seção 9) sem
possuir a chave, porque nada do que a refragmentação altera — tamanho por
fragmento, índice, contagem e o CRC de cada frame — entra no cálculo do tag.

### 8.4 Tag e nível de aplicação

O tag de autenticação tem 16 octetos e **MUST** ser calculado e verificado
no nível da mensagem lógica identificada por (`source_id`, `boot_id`,
`sequence`) (seção 6.2) — antes de fragmentar, no emissor, e depois de
reassemblar todos os fragmentos, no receptor. Um encoder **MUST NOT**
calcular, nem um decoder **MUST NOT** verificar, um tag por fragmento
individual.

O payload lógico cifrado é `ciphertext ‖ tag`: o ciphertext tem o mesmo
tamanho do plaintext (AES-GCM e ChaCha20-Poly1305 são modos de fluxo, sem
padding), seguido imediatamente pelos 16 octetos do tag. Esse payload
lógico, agora 16 octetos maior, é fragmentado pelas regras já existentes
das seções 5 e 9, sem nenhuma mudança de comportamento na fragmentação em
si: o tag é só mais octetos do payload lógico do ponto de vista do
fragmentador.

Um receptor **MUST** rejeitar a mensagem lógica reassemblada, sem entregá-la
ao consumidor, se a verificação do tag falhar — mesma política já aplicada a
um CRC divergente (seção 7): descarte antes de roteamento ou entrega, sem
notificação ativa ao emissor.

### 8.5 Chave

Um endpoint **MUST** usar uma chave de 128 bits (16 octetos) com AES-128-GCM
e uma chave de 256 bits (32 octetos) com ChaCha20-Poly1305 — os dois
tamanhos não são intercambiáveis entre as cifras: a RFC 8439 exige 256 bits
para ChaCha20-Poly1305, sem variante padronizada de 128 bits. Qual dos dois
tamanhos vale em um canal decorre implicitamente do `CIPHER_ID` (seção 8.1)
configurado fora de banda para esse canal.

Essa chave, em qualquer um dos dois tamanhos, **MUST NOT** trafegar em
nenhum campo do wire, em nenhuma mensagem, cifrada ou não. Provisioná-la —
por par de endpoints ou por rede, no tamanho correspondente à cifra
configurada para esse canal — é responsabilidade de cada implantação e está
fora do escopo desta especificação.

`boot_id` **MUST NOT** ser tratado como segredo, nem como derivação ou
substituto da chave: são conceitos independentes. A chave é o segredo
estático de longo prazo; `boot_id`, como qualquer outro campo do cabeçalho,
pode ser lido por qualquer um e só contribui para a unicidade pública do
nonce (seção 8.2).

### 8.6 Estratégia de negociação

`ENCRYPTED` **MUST** ser uma decisão estática de configuração, compartilhada
fora de banda entre os dois endpoints de um canal antes de qualquer frame
ser trocado. Os dois lados já precisam estar provisionados com a mesma
chave (seção 8.5) para que a troca funcione; esta especificação não define
sinalização, descoberta automática nem negociação em tempo de execução — via
`HELLO` ou qualquer outro handshake — para habilitar ou confirmar o uso de
`ENCRYPTED`.

O sub-campo `CIPHER_ID` (seção 5, seção 8.1) não muda essa estratégia. Ele
identifica no wire, frame a frame, qual cifra um emissor que já decidiu
cifrar de fato usou — não é um mecanismo de descoberta, proposta nem
confirmação de capacidade entre os dois lados, e não substitui a
configuração fora de banda dos dois endpoints. A diferença introduzida por
`CIPHER_ID` é apenas de diagnóstico: antes, um decoder que não suportasse a
cifra realmente usada por um emissor não tinha, no próprio wire, como saber
qual cifra deveria tentar; com `CIPHER_ID`, essa identificação deixa de
depender só de configuração externa, ainda que a capacidade de implementar
cada cifra continue sendo, ela sim, combinada fora de banda.

### 8.7 Migração e compatibilidade

Não existe modo legado nem fallback para payload em claro dentro do mesmo
canal enquanto `ENCRYPTED` estiver em uso por um dos lados: um decoder
**MUST** tratar cada frame de acordo com o bit `ENCRYPTED` que o próprio
frame declara, e um mismatch de configuração entre os dois endpoints de um
canal — um lado emitindo com `ENCRYPTED` marcado e o outro esperando payload
em claro, ou vice-versa — é erro de implantação, não caso previsto de
negociação de wire; não é responsabilidade do decoder detectar ou
recuperar-se dessa condição além da rejeição já exigida por CRC ou tag
inválidos (seções 7 e 8.4).

Esta especificação não define, por ora, uma política de `ENCRYPTED` para o
perfil `UsbHid`: um encoder **MUST NOT** marcar `ENCRYPTED` em um frame
destinado a esse perfil (`BTP_USB_HID_MAX_PAYLOAD_SIZE`, seção 9); os 16
octetos do tag sobre um payload de 22 octetos ficam reservados para revisão
futura desta especificação.

## 9. Limites normativos por transporte

As constantes abaixo fazem parte do contrato do wire v1:

| Constante | Valor |
| --- | ---: |
| `BTP_V1_HEADER_SIZE` | 36 |
| `BTP_V1_CRC_SIZE` | 4 |
| `BTP_ESPNOW_MAX_FRAME_SIZE` | 250 |
| `BTP_ESPNOW_MAX_PAYLOAD_SIZE` | 210 |
| `BTP_SERIAL_MAX_FRAME_SIZE` | 4096 |
| `BTP_SERIAL_MAX_PAYLOAD_SIZE` | 4056 |
| `BTP_USB_HID_MAX_FRAME_SIZE` | 62 |
| `BTP_USB_HID_MAX_PAYLOAD_SIZE` | 22 |

Um frame destinado a ESP-NOW **MUST** satisfazer `frame_size <= 250` e
`payload_size <= 210`. Cada datagrama contém somente os 36 octetos do header,
os `payload_size` octetos válidos e os 4 octetos do CRC. O emissor **MUST NOT**
completar o datagrama com zeros ou transmitir uma área fixa de 250 octetos.
Assim, o tamanho do datagrama **MUST** ser exatamente `40 + payload_size`.

Um frame BTP decodificado destinado à serial protocolada **MUST** satisfazer
`frame_size <= 4096` e `payload_size <= 4056`, antes da codificação de framing
da serial. A codificação COBS e seus delimitadores são definidos em
[`TRANSPORT_SERIAL.md`](TRANSPORT_SERIAL.md) e não contam em `frame_size`. O
mapeamento de um frame para ESP-NOW é definido em
[`TRANSPORT_ESPNOW.md`](TRANSPORT_ESPNOW.md).

Um frame destinado a USB HID **MUST** satisfazer `frame_size <= 62` e
`payload_size <= 22`. O relatório HID físico tem 64 octetos; um octeto é
reservado ao Report ID e um octeto ao prefixo de tamanho que distingue dados
reais de padding (`TRANSPORT_USB_HID.md` seção 2), restando exatamente 62
para o frame BTP inteiro. O mapeamento de um frame para relatórios HID é
definido em [`TRANSPORT_USB_HID.md`](TRANSPORT_USB_HID.md).

Uma mensagem lógica maior que o payload permitido pelo transporte **MUST** ser
fragmentada. Um encoder **MUST** verificar o limite do transporte antes de
escrever ou enviar. Um decoder **MUST** rejeitar o frame se o tamanho declarado
exceder o limite do transporte pelo qual ele foi recebido.

## 10. Validação de um frame

Sem ler além do buffer fornecido, um decoder de wire v1 **MUST**:

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

## 11. Exemplos hexadecimais

Os exemplos desta seção demonstram a validade do envelope. Documentos de canal
posteriores **MAY** impor requisitos adicionais ao conteúdo do payload.

### 11.1 LOG sem payload

Campos principais: `type=LOG`, `source_id=0x11223344`,
`boot_id=0xA1B2C3D4`, `sequence=1`, `timestamp_us=1000000`, `object_id=2`,
sem fragmentação e sem payload.

```text
42 54 50 00  01 02  00 00  24 00  00 00
44 33 22 11  d4 c3 b2 a1  01 00 00 00
40 42 0f 00 00 00 00 00  02 00  00 01
3a 15 e7 df
```

O CRC numérico é `0xDFE7153A`; no wire little-endian aparece como
`3a 15 e7 df`. O frame possui 40 octetos.

### 11.2 Segundo fragmento de TELEMETRY

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
