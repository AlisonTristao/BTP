# Codec BTP v1

## API portátil

O codec canônico está em [`include/btp/codec.hpp`](../include/btp/codec.hpp) e
é implementado por [`src/codec.cpp`](../src/codec.cpp). A API exige somente
C++11 e cabeçalhos da biblioteca padrão para tipos inteiros e tamanhos; não
depende de Arduino, ESP-IDF, Qt ou sistema operacional.

COBS, stream serial, fragmentação e reassembly fazem parte do mesmo alvo de
biblioteca e estão documentados em
[`STREAM_AND_REASSEMBLY.md`](STREAM_AND_REASSEMBLY.md).
Os frames binários de referência e a forma de executá-los em cada consumidor
estão em [`CONFORMANCE.md`](CONFORMANCE.md).

O núcleo não aloca memória. Payloads são representados por `ByteView`, isto é,
um ponteiro para octetos e um tamanho explícito. O encoder escreve em um buffer
fornecido pelo chamador. O decoder devolve uma view apontando para dentro do
frame de entrada, portanto o chamador deve manter esse buffer válido enquanto
usar o resultado.

O payload de entrada pode estar no mesmo buffer da saída: o encoder suporta
sobreposição e pode, por exemplo, deslocar bytes para prefixar o header no
próprio buffer.

As constantes `kLibraryVersion*`, `kMinimumProtocolVersion` e
`kMaximumProtocolVersion` identificam a implementação `0.1.0` em elaboração e
o intervalo de wire versions aceito, atualmente somente BTP v1. Esses números
não representam uma release publicada ou uma tag Git.

## Encode

O chamador primeiro usa `encoded_size(payload_size, transport, &size)` para
obter o tamanho exato e validar o limite do transporte. Depois preenche
`Header` e `ByteView` e chama `encode`. Todos os argumentos, invariantes do
cabeçalho, limites e a capacidade de saída são validados antes da primeira
escrita. Em erro, o conteúdo do buffer e `bytes_written` permanecem intocados.

Exemplo abreviado:

```cpp
#include <btp/codec.hpp>

std::uint8_t payload[] = {0x00, 0x0a, 0x0d, 0xff};
std::uint8_t output[btp::kEspNowMaxFrameSize];

btp::Header header = {};
header.type = btp::MessageType::Telemetry;
header.source_id = 1;
header.boot_id = 1;
header.fragment_count = 1;

btp::Frame frame = {header, {payload, sizeof(payload)}};
std::size_t written = 0;
btp::Error result = btp::encode(
    frame, btp::TransportProfile::EspNow,
    output, sizeof(output), &written);
```

## Decode e ordem de validação

`decode` recebe obrigatoriamente o perfil do enlace pelo qual o frame chegou.
Isso impede que um frame serial de 4096 octetos seja aceito acidentalmente em
ESP-NOW. Antes de publicar `DecodedFrame`, o codec valida:

1. argumentos e limites acessíveis do buffer;
2. magic, versão e tamanho fixo do header;
3. tamanho declarado e limite do transporte;
4. CRC sobre header e payload;
5. tipo, flags, IDs e invariantes de fragmentação.

Em falha, `DecodedFrame` permanece intocado. `error_string` converte cada
`Error` em texto estático para diagnóstico; nenhum fallback ou parser legado é
tentado.

## Build e testes

No desktop, a partir da raiz:

```text
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

O alvo `btp::codec` pode ser ligado por outro projeto CMake. Para PlatformIO,
`library.json` permite referenciar esta raiz como dependência. O smoke build
embarcado usado pelo repositório é executado com:

```text
pio run -d tests/embedded
```

A suíte do codec cobre os dois exemplos hexadecimais da especificação, o check CRC de
`123456789`, payload vazio e máximo, round trip byte a byte, truncamentos,
limites de transporte, corrupção de cada campo e rejeições semânticas com CRC
recalculado. A suíte de transporte, executada pelo mesmo `ctest`, cobre COBS,
decoder serial incremental, fragmentação e reassembly.
`btp_conformance_tests` compara o codec com os arquivos canônicos e
`btp_vector_descriptions` prova que cada `.bin` é derivado de sua descrição
JSON sem depender de layout de memória, alinhamento ou compilador.
