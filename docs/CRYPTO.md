# Criptografia AEAD do payload

## Escopo deste capítulo

O BTP v2 acrescenta uma única capacidade ao protocolo: o payload lógico de
uma mensagem pode ir cifrado e autenticado, sem que nada no envelope, no
roteamento ou nos perfis de transporte mude de significado. Este capítulo
explica o modelo por trás disso, o raciocínio de cada escolha e a API que
implementa a cifra neste repositório.

A norma vive em outro lugar: a seção 8 de [`BTP_V1.md`](BTP_V1.md) é a fonte
canônica dos requisitos **MUST**/**SHOULD**, e a
[ADR 0012](decisions/0012-criptografia-aead-payload.md) registra a decisão,
as alternativas rejeitadas e as consequências. Divergências entre este
capítulo e a seção 8 resolvem-se a favor da seção 8.

## O problema: CRC não é autenticação

O CRC-32 de cada frame detecta corrupção acidental de transporte. Ele não faz
— e nunca pretendeu fazer — nada contra alteração intencional: um atacante
que muda um octeto do payload recalcula o CRC em uma linha de código.

O que torna isso concreto é o enlace ESP-NOW entre robô e dongle: rádio
aberto, onde qualquer um no alcance captura o tráfego passivamente e injeta
frames válidos sem precisar de acesso físico a nada. Os perfis USB são
diferentes na origem do risco — interceptar `Serial` ou `UsbHid` exige posse
física do dongle —, e é por isso que a política de criptografia deles não
mudou.

## O que a v2 garante, e o que não garante

| Garantia | Estado |
| --- | --- |
| Confidencialidade do payload lógico | Sim, quando `ENCRYPTED` está marcado. |
| Integridade e autenticidade do payload | Sim: tag de 16 octetos sobre o ciphertext. |
| Integridade e autenticidade do header | Sim, via AAD — o header vai em claro, mas alterá-lo invalida o tag. |
| Confidencialidade dos metadados do header | **Não.** `type`, `object_id`, `source_id`, `timestamp_us` e tamanhos são legíveis por qualquer observador, por construção: o gateway precisa deles para rotear antes de decifrar. |
| Proteção contra replay | **Não coberta por esta versão.** Um frame válido capturado e reenviado tem tag e CRC válidos; recusar uma tripla (`source_id`, `boot_id`, `sequence`) já vista é decisão do receptor, e a especificação ainda não a exige. |
| Rotação de chave, forward secrecy | **Não.** A chave é estática, provisionada fora de banda; não há negociação, derivação por sessão nem renovação definidas. |
| Autenticação de identidade | Só por posse da chave. Quem tem a chave do canal pode emitir frames que autenticam como qualquer `source_id`. |
| Perfil `UsbHid` | Fora de escopo: um encoder **MUST NOT** marcar `ENCRYPTED` nesse perfil. |

As três lacunas marcadas como "não" são deliberadas e documentadas, não
esquecimentos — mas quem for construir sobre isso precisa saber que elas
existem antes de assumir o contrário.

## Como o wire muda

Quando o bit `ENCRYPTED` está marcado, três coisas acontecem no envelope, e
nada mais:

```text
version      0x01  ->  0x02          selecionado pelo encoder, não pelo chamador
flags        bit 0x0002 marcado      ENCRYPTED
             bits 2-3 = CIPHER_ID    qual das duas cifras produziu o payload
payload      plaintext  ->  ciphertext ‖ tag       (+16 octetos por mensagem)
```

O header continua em claro, o CRC continua sendo calculado sobre o frame
inteiro como antes, e a fragmentação continua operando sobre o payload lógico
sem saber que ele está cifrado. Com `ENCRYPTED` limpo, o frame é byte a byte
idêntico ao de um encoder v1.

### As duas cifras

| `CIPHER_ID` | Cifra | Chave | Quando usar |
| ---: | --- | ---: | --- |
| `0` | AES-128-GCM | 16 octetos | Padrão. Acelerada em hardware nos ESP32-S3 do projeto e via AES-NI no desktop. |
| `1` | ChaCha20-Poly1305 | 32 octetos | Endpoint sem acelerador AES: constante em tempo por construção, sem depender de hardware. |
| `2`, `3` | Reservado | — | Rejeitado por um decoder desta versão. |

Os dois tamanhos de chave **não são intercambiáveis**: a RFC 8439 exige 256
bits para ChaCha20-Poly1305 e não define variante de 128 bits. `CIPHER_ID`,
portanto, determina também o tamanho da chave que aquele canal usa — o que o
provisionamento precisa saber antes de gerar a chave.

`CIPHER_ID` é um sub-campo de 2 bits, e não duas flags booleanas, para que o
wire não consiga representar estado ambíguo: com duas flags existiriam as
combinações "as duas cifras marcadas" e "`ENCRYPTED` ligado e nenhuma cifra
marcada". Com um valor de 1-em-4 esses estados simplesmente não existem.

Duas consistências são normativas e verificadas pelo decoder:

- `ENCRYPTED` marcado com `version == 0x01` é rejeitado
  (`Error::EncryptedVersionMismatch`);
- `CIPHER_ID` diferente de zero com `ENCRYPTED` limpo é rejeitado
  (`Error::InvalidCipherId`) — um frame não cifrado não tem cifra "em uso".

### O nonce sai do header

As duas cifras exigem 96 bits de nonce, único por mensagem sob uma mesma
chave. O BTP não acrescenta campo nenhum para isso:

```text
nonce = source_id (4) ‖ boot_id (4) ‖ sequence (4)
```

A unicidade não é uma esperança, é consequência de regras que o protocolo já
exigia antes da criptografia existir: `source_id` é único no domínio de
roteamento, `sequence` não se repete dentro de um boot, e `boot_id` é novo a
cada inicialização.

O papel do `boot_id` aqui é o menos óbvio e o mais importante. Sem ele, um
reboot reiniciaria `sequence` a partir de valores já usados e reutilizaria
nonces sob a mesma chave — o cenário catastrófico para GCM, que permite
recuperar keystream e forjar tags. Com ele, cada boot ocupa uma faixa de
nonce disjunta de todas as anteriores.

`boot_id` **não** é segredo, nem derivação da chave: como qualquer campo do
header, ele é público. Nonce público é normal; o que não pode vazar é a
chave.

### O AAD é o header inteiro

Os 36 octetos do header entram na operação AEAD como dados associados. Eles
seguem em claro no wire — o pipeline de validação e roteamento precisa ler
`type` e `object_id` antes de qualquer decifragem — mas qualquer bit alterado
neles invalida o tag exatamente como alterar o ciphertext invalidaria.

O `payload_size` gravado nesse header é o tamanho **do wire**, isto é, do
`ciphertext ‖ tag` — não o tamanho do plaintext. É por isso que
`aead_seal()` recebe o tamanho do plaintext e soma 16 internamente antes de
montar o AAD, enquanto `aead_open()` recebe um tamanho que já inclui o tag e
o usa como está.

### O AAD de uma mensagem fragmentada

Aqui está a parte que é fácil errar. O tag é calculado **uma vez**, antes de
fragmentar, mas cada fragmento carrega no wire um header **diferente** —
`payload_size` da sua fatia, seu `fragment_index`, o bit `FRAGMENTED`
marcado. Nenhum desses headers é o AAD. O AAD é o header de uma coisa que
nunca aparece no wire: a mensagem lógica.

E aí surge a pergunta que decide se a coisa funciona: quais valores vão nos
campos de fragmentação desse header? Cada resposta dá 36 octetos diferentes,
logo um tag diferente. Se os dois lados escolherem respostas diferentes,
**toda** mensagem fragmentada falha na verificação — de um jeito
indistinguível de chave errada ou de ataque.

A regra normativa (seção 8.3) é canonicalizar os três campos que variam entre
fragmentos:

| Campo | Valor no AAD |
| --- | --- |
| `payload_size` | o do `ciphertext ‖ tag` completo, não o da fatia |
| `flags` | os flags do frame com `FRAGMENTED` limpo |
| `fragment_index` / `fragment_count` | `0` e `1` |

Todo o resto entra no AAD com o valor que está no wire. Como nenhum outro
campo varia dentro de uma mensagem lógica, o receptor reconstrói o AAD a
partir do header de **qualquer** fragmento — não precisa guardar o primeiro
nem tratar o índice 0 de forma especial.

O `btp::aead` faz essa canonicalização dentro de `aead_seal()`/`aead_open()`,
não por contrato com o chamador: passar o header de um fragmento ou o da
mensagem lógica produz exatamente o mesmo tag, e é isso que
`test_aad_ignores_fragmentation_fields` verifica byte a byte. Para mensagem
não fragmentada é um no-op — os campos já valem `0`/`1` —, então todos os
vetores de conformidade existentes seguem produzindo o mesmo AAD de antes.

O ganho não é só evitar a ambiguidade: como o tag não depende do
enquadramento, **o gateway pode refragmentar uma mensagem cifrada sem ter a
chave**. Isso importa concretamente, porque o dongle roteia entre perfis com
tetos diferentes (210 octetos no ESP-NOW, 22 no USB HID). Se os campos de
fragmentação entrassem no AAD, reenquadrar exigiria recifrar, e o dongle
precisaria da chave apenas para trocar de enlace.

### O tag pertence à mensagem, não ao fragmento

```text
emissor                                    receptor
-------                                    --------
payload lógico em claro                    fragmentos recebidos
   |                                          |
   v                                          v
aead_seal  ->  ciphertext ‖ tag            reassembly  ->  ciphertext ‖ tag
   |                                          |
   v                                          v
fragmenta  ->  N frames + CRC cada         aead_open   ->  payload em claro
```

Cifrar antes de fragmentar, verificar depois de remontar. A razão é a
identidade: `sequence` identifica a mensagem lógica, não o frame físico. Um
tag por fragmento exigiria enfiar `fragment_index` no nonce e criaria mais
uma forma de errar, sem ganho — e o CRC por frame já cobre o que precisa ser
coberto barato, antes do reassembly.

Tag inválido é mensagem descartada antes de qualquer entrega ao consumidor,
sem notificação ao emissor: mesma política do CRC divergente.

### Custo

O tag custa 16 octetos por **mensagem lógica**, não por fragmento; o
ciphertext tem o mesmo tamanho do plaintext, porque os dois modos são de
fluxo e não têm padding.

| Perfil | Payload máximo | Overhead do tag |
| --- | ---: | ---: |
| `Serial` | 4056 octetos | ~0,4% |
| `EspNow` | 210 octetos | ~7,6% |
| `UsbHid` | 22 octetos | 73% — motivo de o perfil estar fora de escopo |

No ESP-NOW isso pode empurrar uma mensagem que já estava perto do teto para
um fragmento a mais, mas não muda o comportamento da fragmentação em si.

## A API `btp::aead`

A cifra vive em [`include/btp/aead.hpp`](../include/btp/aead.hpp) e
[`src/aead.cpp`](../src/aead.cpp), em um alvo próprio — `btp::aead` —
separado de `btp::codec`. A separação é deliberada e vale a pena entender:

- `btp::codec` **não tem nenhuma dependência externa** e não vai ganhar
  nenhuma. Ele trata um payload cifrado como bytes opacos: valida framing,
  CRC, flags e fragmentação, e nunca chama uma cifra. Um consumidor que só
  precisa de framing continua compilando sem mbedtls.
- `btp::aead` é o único alvo que depende de mbedtls, e depende dele de forma
  privada.
- Por isso o erro de AEAD tem enum próprio (`AeadError`) em vez de estender
  `btp::Error`: vocabulário de cripto não entra no header sem dependências.

### Funções

| Função | O que faz |
| --- | --- |
| `aead_seal()` / `aead_open()` | Despacham para a cifra indicada por `CIPHER_ID` em `header.flags`. É a entrada recomendada: quem chama não precisa saber qual cifra o canal usa. Um `CIPHER_ID` reservado devolve `AeadError::InvalidCipherId`. |
| `aead_seal_aes_gcm()` / `aead_open_aes_gcm()` | AES-128-GCM direto, sem despacho. |
| `aead_seal_chacha20poly1305()` / `aead_open_chacha20poly1305()` | ChaCha20-Poly1305 direto, sem despacho. |

Convenções compartilhadas por todas elas, na mesma linha do resto da
biblioteca — sem alocação, sem posse de memória, tudo em buffer do chamador:

- `AeadKey` é uma *view* (ponteiro + tamanho), não dona da chave, porque as
  duas cifras usam tamanhos diferentes e um tipo de tamanho fixo não serviria
  às duas. O tamanho é validado contra a cifra escolhida: `kAesGcmKeySize`
  (16) ou `kChaCha20Poly1305KeySize` (32).
- No *seal*, `payload_size` é o tamanho do plaintext e o buffer de saída
  precisa de `payload_size + 16` octetos.
- No *open*, `ciphertext_size` **já inclui** os 16 octetos do tag, e o buffer
  de saída precisa de `ciphertext_size - 16`.
- `AeadError::TagMismatch` distingue "não autentica" de
  `AeadError::InvalidArgument` ("nem deu para tentar").

### Cifrar e enviar

```cpp
#include "btp/aead.hpp"
#include "btp/codec.hpp"

// Chave provisionada fora de banda; 16 octetos porque CIPHER_ID = 0.
const std::uint8_t key_bytes[btp::kAesGcmKeySize] = { /* ... */ };
const btp::AeadKey key{key_bytes, sizeof(key_bytes)};

btp::Header header;
header.type = btp::MessageType::Telemetry;
header.flags = btp::kFlagEncrypted;   // CIPHER_ID = 0 -> AES-128-GCM
header.source_id = robot_source_id;   // != 0
header.boot_id = this_boot_id;        // != 0, novo a cada boot
header.sequence = next_sequence++;    // não repete neste boot
header.timestamp_us = monotonic_micros();
header.object_id = topic_id;
header.fragment_index = 0U;
header.fragment_count = 1U;

std::uint8_t sealed[sizeof(sample) + 16U];
if (btp::aead_seal(key, header, sizeof(sample), sample, sealed) != btp::AeadError::Ok) {
    return;  // chave de tamanho errado, CIPHER_ID reservado ou header inválido
}

// sealed é o payload lógico agora: fragmentar (se preciso) e encode() como sempre.
const btp::Frame frame{header, {sealed, sizeof(sealed)}};
std::size_t written = 0U;
btp::encode(frame, btp::TransportProfile::EspNow, out, sizeof(out), &written);
```

### Receber e decifrar

```cpp
btp::DecodedFrame decoded;
if (btp::decode(input, input_size, btp::TransportProfile::EspNow, &decoded) != btp::Error::Ok) {
    return;  // framing, CRC, flags ou fragmentação inválidos
}

// Só depois de remontar todos os fragmentos da mensagem lógica:
std::uint8_t plaintext[kMaxPlaintext];
const btp::AeadError rc = btp::aead_open(
    key, decoded.header, static_cast<std::uint16_t>(decoded.payload.size),
    decoded.payload.data, plaintext);

if (rc == btp::AeadError::TagMismatch) {
    return;  // descartar sem entregar e sem notificar o emissor
}
```

`decode()` valida o framing e para aí: ele aceita um frame cujo tag não
autentica, porque não tem como saber disso. A verificação criptográfica é uma
camada acima, e é responsabilidade de quem consome chamar `aead_open()`.

## Build

A dependência de cripto é opcional e isolada:

```text
cmake -S . -B build -G Ninja                         # BTP_ENABLE_AEAD=ON por padrão
cmake -S . -B build -G Ninja -DBTP_ENABLE_AEAD=OFF   # sem mbedtls, sem btp::aead
cmake --build build
ctest --test-dir build --output-on-failure
```

Com `BTP_ENABLE_AEAD=ON`, o CMake baixa o mbedtls por `FetchContent`, fixado
em uma tag (nunca em uma branch), e constrói o alvo `btp_aead`/`btp::aead`
linkado privadamente contra `mbedcrypto`. Isso evita exigir mbedtls instalado
no sistema para o build nativo; nas plataformas embarcadas ele já vem do
framework ESP-IDF/Arduino. Com a opção desligada, a dependência e o alvo
desaparecem por completo, e `btp::codec` compila exatamente como antes.

Dois testes cobrem esta camada: `btp_aead_tests` (round-trip das duas
cifras, tag corrompido, AAD alterado, chave de tamanho errado, `CIPHER_ID`
reservado) e `btp_aead_conformance_tests` (os vetores v2, abaixo).

## Prova de interoperabilidade

[`test-vectors/v2/`](../test-vectors/v2/README.md) tem dois vetores válidos
cujo `payload_hex` é `ciphertext ‖ tag` **real** — um por cifra, gerados com
o pacote Python `cryptography`, com chave, nonce, AAD e plaintext
documentados no bloco `"aead"` de cada `.json`.
[`tests/test_aead_conformance.cpp`](../tests/test_aead_conformance.cpp)
decodifica cada `.bin` com `btp::decode()` e então chama o `btp::aead` de
verdade sobre o payload, comparando o plaintext recuperado com o documentado.

Isso fecha uma lacuna que costuma passar despercebida: não é só o framing que
bate "no papel" — o ciphertext produzido por uma implementação Python é
decifrado corretamente pela implementação C++ deste repositório. É esse tipo
de prova que um novo consumidor deve reproduzir antes de se dizer conforme.

A suíte de framing (`test_conformance_v2.cpp`) cobre, do outro lado, os casos
inválidos que o decoder **deve** rejeitar sem saber nada de cripto:
`ENCRYPTED` com versão 1, `CIPHER_ID` reservado, `CIPHER_ID` sem
`ENCRYPTED`, bit reservado, CRC corrompido em frame cifrado. Casos de "tag
corrompido, mas estruturalmente válido" não pertencem ali — `decode()` os
aceita por definição — e vivem em `test_aead_conformance.cpp`.

## Estado e o que falta

A [ADR 0012](decisions/0012-criptografia-aead-payload.md) segue como
`Proposta`, e é correto que siga: a especificação está escrita, a biblioteca
implementa as duas cifras e os vetores provam interoperabilidade, mas os três
consumidores do protocolo — `bally_OS`, `bally_dongle` e `TraceView` — ainda
não chamam cifra nenhuma. Enquanto isso não acontecer, a decisão não está
concluída pelo critério do próprio
[`CONTRIBUTING.md`](CONTRIBUTING.md): "uma mudança não está completa se
apenas um consumidor consegue codificá-la".

Ainda em aberto, em ordem de impacto:

1. **Provisionamento de chave.** Obrigatório operacionalmente e fora do wire
   por decisão: NVS/flash nos dois lados do ESP-NOW, configuração local no
   TraceView. Sem isso, nada disso liga em produção.
2. **Um vetor de conformidade de mensagem fragmentada e cifrada.** A
   canonicalização do AAD está normativa, implementada e coberta por teste
   unitário, mas ainda não existe um par `.json`/`.bin` que a prove
   entre plataformas, como os dois vetores de mensagem única já provam.
3. **A política de `ENCRYPTED` para `UsbHid`**, provavelmente com tag
   truncado, em uma ADR futura.
4. **Anti-replay**, se e quando for considerado necessário — hoje não há
   requisito escrito.

Classificação de versão: `MAJOR`, `v2.0.0`, porque um decoder v1.x **MUST**
rejeitar frame com bit reservado marcado — logo a extensão não é
seguramente ignorável por um peer antigo, mesmo sendo opcional em uso. A
linha v1 continua na branch `1.x`.
