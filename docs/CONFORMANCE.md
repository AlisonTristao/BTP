# Vetores de conformidade

## Contrato dos arquivos

[`test-vectors/v1/`](../test-vectors/v1/) é a referência binária canônica do
wire v1. Cada vetor de frame possui dois arquivos com o mesmo nome:

- `.json`: descrição legível por máquina e por pessoa, com campos de largura
  fixa expressos por valor e payload expresso como octetos hexadecimais;
- `.bin`: frame BTP cru, da magic ao CRC inclusive, sem COBS, delimitadores
  seriais, padding ou wrapper ESP-NOW.

O campo `transport` do JSON escolhe os limites usados para validar o frame. O
conteúdo binário do frame permanece o mesmo nos dois transportes. A
codificação COBS não faz parte destes `.bin` porque é framing do stream, não do
envelope BTP.

[`manifest.json`](../test-vectors/v1/manifest.json) enumera todos os vetores e
a ordem de chegada do cenário com duas fontes fragmentadas. A suíte cobre:

- `HELLO`, `LOG` UTF-8, `TELEMETRY` `PACKED_LE`, `protocol.test` canônico e
  `COMMAND_REQUEST` válidos;
- o array `00 0a 0d 7f 80 ff` dentro de payload binário;
- fragmentos fora de ordem de duas fontes, intercalados sem mistura;
- magic, versão, `header_size`, `payload_size`, CRC, índice e contagem de
  fragmento inválidos;
- o motivo exato pelo qual cada frame inválido deve falhar.

Os JSONs em `valid/` descrevem o envelope inteiro e o modelo humano do payload.
Os JSONs em `invalid/` referenciam um vetor válido, declaram uma mutação exata
por offset e registram `expected_error`. Mutações sem `recompute_crc` exercitam
a ordem normativa de validação; as inconsistências semânticas de fragmentação
recalculam o CRC para alcançar a validação do header.

## Executar a suíte canônica

Na raiz de `BTP`:

```text
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`btp_vector_descriptions` gera cada frame em memória somente a partir do JSON,
com escrita little-endian explícita, e compara todos os octetos com o `.bin`.
Também decodifica o frame de forma independente, compara todos os campos e
payload, valida o CRC e confirma o erro documentado de cada caso inválido.

`btp_conformance_tests` usa a implementação C++ real: codifica as descrições
compiladas e compara os `.bin` byte a byte, decodifica cada `.bin` e compara
todos os campos, exige o erro exato dos inválidos e executa o reassembly das
duas fontes intercaladas.

Para verificar apenas que JSON e `.bin` continuam sincronizados:

```text
python tools/test_vectors.py --root test-vectors/v1 --check
```

## Uso por ESP-IDF, Arduino e Qt

O consumidor deve fixar uma tag ou revisão imutável de `BTP` e usar
os arquivos diretamente dessa dependência. Não deve copiar ou manter uma
variante local dos vetores.

Em testes de desktop, leia `.bin` como bytes, sem conversão de texto. Em testes
embarcados, o build pode transformar o mesmo `.bin` da dependência em um array
`uint8_t` ou gravá-lo em uma partição de teste; o arquivo de origem continua
sendo este. Nunca derive o tamanho com `sizeof` de uma estrutura de protocolo:
o tamanho esperado é a quantidade de octetos do arquivo.

Cada implementação deve, no mínimo:

1. codificar os campos do JSON e exigir igualdade byte a byte com o `.bin`;
2. decodificar o `.bin` e comparar tipo, flags, IDs, sequência, timestamp,
   objeto, fragmentação, payload e CRC;
3. rejeitar cada vetor inválido pelo `expected_error` documentado;
4. alimentar a ordem de `manifest.json` no reassembler e comparar os dois
   payloads lógicos completos;
5. executar isso nas toolchains realmente usadas por ESP-IDF, Arduino e Qt.

O nome da enum de erro pode variar entre linguagens, mas deve mapear sem
ambiguidade para o motivo BTP documentado. Um erro anterior ou genérico não
conta como conformidade quando impedir a distinção exigida pelo contrato.

## Alterar um vetor é alterar o contrato

Arquivos `.bin` não são editados manualmente. Para uma mudança intencional:

1. classifique o impacto conforme [`VERSIONING.md`](VERSIONING.md);
2. atualize especificação e ADR quando bytes, interpretação, limites ou
   garantias mudarem;
3. altere ou acrescente a descrição JSON;
4. gere os binários com o comando abaixo;
5. revise o diff hexadecimal e execute toda a suíte nas três plataformas;
6. registre migração e mudança de contrato na release correspondente.

```text
python tools/test_vectors.py --root test-vectors/v1
```

Uma alteração acidental de JSON ou `.bin` falha em `--check`. Mesmo uma troca
que preserve o tamanho é uma mudança observável e não pode ser aceita como
simples atualização de fixture.
