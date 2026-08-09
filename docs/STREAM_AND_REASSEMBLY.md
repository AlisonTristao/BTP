# COBS, fragmentação e reassembly

## Escopo

A implementação portável está dividida em:

- [`btp/stream.hpp`](../include/btp/stream.hpp): COBS e decoder serial
  incremental;
- [`btp/fragmentation.hpp`](../include/btp/fragmentation.hpp): fragmentador e
  reassembly de mensagens lógicas.

Esses componentes exigem C++11, não usam heap e escrevem apenas em memória
fornecida pelo chamador.

## COBS e stream serial

`cobs_encode` e `cobs_decode` recebem buffers não sobrepostos. Ambos validam
argumentos, encoding e capacidade antes de publicar `bytes_written`; o decode
também valida o bloco inteiro antes de escrever a saída. Payload vazio é
codificado como `01`.

`cobs_max_encoded_size` fornece o limite superior
`N + floor(N / 254) + 1`. Para o frame serial máximo de 4096 octetos, a
constante `kSerialMaxCobsBlockSize` vale 4113.

`SerialDecoder` recebe na construção um buffer codificado de ao menos 4113
octetos e um buffer decodificado de ao menos 4096. `push` consome um octeto por
vez e reporta:

| Evento | Significado |
| --- | --- |
| `None` | ainda não existe resultado para o consumidor |
| `Frame` | COBS e frame BTP foram validados; `DecodedFrame` pode ser usado |
| `CobsError` | o candidato atual tinha COBS inválido ou saída excessiva |
| `FrameError` | COBS válido, mas envelope BTP inválido; `frame_error` dá o motivo |
| `Overflow` | o bloco ultrapassou 4113 octetos; bytes são descartados até zero |
| `InvalidConfiguration` | buffers ou argumentos não satisfazem o contrato |

O decoder começa esperando um delimitador. Depois de qualquer candidato
inválido ou overflow, o zero seguinte restaura a coleta, de modo que um frame
válido posterior não depende de reinicialização da porta. Delimitadores
consecutivos não produzem frame vazio. O payload retornado aponta para o buffer
decodificado e permanece válido até outro candidato ser concluído.

## Fragmentador

`fragment_count` calcula quantos frames o payload exige no perfil selecionado.
O limite é 255 fragmentos; uma mensagem maior é rejeitada com
`PayloadTooLarge`.

`make_fragment` retorna uma view zero-copy para o trecho solicitado. Para uma
mensagem que cabe em um frame, normaliza `flags`, `fragment_index=0` e
`fragment_count=1`. Para duas ou mais partes, marca `FRAGMENTED` e preserva
`type`, `source_id`, `boot_id`, `sequence`, `timestamp_us` e `object_id` em
todas elas. O chamador ainda codifica cada `Frame` normalmente, obtendo CRC
independente por fragmento.

## Reassembly limitado

O chamador instancia uma quantidade fixa de `ReassemblySlot` e fornece um
`ReassemblyStorage` de capacidade fixa para cada slot. Cada slot mantém uma
mensagem identificada pela tripla `(source_id, boot_id, sequence)`. Não há
reserva, realocação ou crescimento automático.

`Reassembler::push` aceita fragmentos fora de ordem. Os bytes recebidos são
mantidos contíguos e ordenados dentro do storage. A mensagem só é publicada
como `Complete` quando todos os índices foram recebidos; o header publicado é
normalizado como uma mensagem não fragmentada.

As regras de falha são:

- repetição do mesmo índice e dos mesmos bytes retorna `Duplicate` e não
  renova o timeout;
- repetição com conteúdo diferente retorna `Conflict` e invalida o slot;
- divergência de tipo, flags, timestamp, objeto ou total para a mesma
  identidade também invalida o slot;
- índice fora do intervalo retorna `InvalidFragment`;
- soma maior que o storage retorna `MessageTooLarge` e libera o slot;
- todos os slots ocupados retorna `NoSlot` sem desalojar mensagem válida.

`expire(now_ms)` libera de forma determinística slots sem progresso durante o
timeout configurado. `clear()` descarta todos os reassemblies, por exemplo ao
encerrar uma sessão. Uma mensagem completa continua ocupando seu slot para
manter a `ByteView` estável; o consumidor deve chamar `release(slot_index)`
quando terminar. Conflitos posteriores não alteram um slot completo; slots
completos também expiram se não forem liberados.

Exemplo de configuração com duas mensagens simultâneas de até 1024 octetos:

```cpp
btp::ReassemblySlot slots[2];
std::uint8_t bytes_a[1024];
std::uint8_t bytes_b[1024];
const btp::ReassemblyStorage storage[2] = {
    {bytes_a, sizeof(bytes_a)},
    {bytes_b, sizeof(bytes_b)}
};
btp::Reassembler reassembler(slots, storage, 2, 1000);
```

O payload passado a `push` deve vir do frame recebido, e não de dentro do
storage pertencente ao próprio reassembler.

## Testes

`tests/test_transport.cpp` cobre vazio, zeros consecutivos, todos os valores de
byte, frame serial máximo, ressincronização depois de ruído, COBS inválido,
CRC inválido, frame parcial e overflow, além de fragmentos duplicados, conflitantes, fora de
ordem, duas fontes intercaladas, limite de storage, timeout e liberação.
