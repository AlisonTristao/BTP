# Como contribuir com o BTP

## Princípios obrigatórios

- Preserve este repositório como fonte canônica. Não copie especificação,
  codec ou vetores para repositórios consumidores.
- Não adicione suporte legado, adaptadores, fallback ou autodetecção do
  protocolo anterior.
- Não transmita representação de memória de `struct`.
- Trate payload como bytes com tamanho explícito, nunca como string terminada.
- Mantenha `LOG`, `TELEMETRY`, `COMMAND` e `TERMINAL` semanticamente separados.

## Antes de alterar

1. Leia os documentos deste diretório e os ADRs relacionados à área que você vai
   alterar.
2. Verifique o estado Git e preserve alterações existentes.
3. Delimite se a mudança afeta documentação, API pública, wire format ou mais
   de uma dessas áreas.

Não crie tags, releases, pacotes ou commits em nome de outra pessoa sem pedido
explícito.

## Processo para mudar o wire format

Toda proposta que mude bytes, interpretação, limites ou garantias do wire deve
incluir no mesmo conjunto de mudanças:

1. motivação e impacto nos três consumidores;
2. um ADR novo, ou a marcação de um ADR anterior como substituído;
3. atualização da especificação canônica antes ou junto do código;
4. classificação `MAJOR`, `MINOR` ou `PATCH` conforme [`VERSIONING.md`](VERSIONING.md);
5. vetores de conformidade válidos e inválidos, legíveis por máquina;
6. implementação e testes equivalentes nas plataformas afetadas;
7. estratégia explícita de negociação para extensões compatíveis;
8. notas de migração sem caminho de compatibilidade legada.

Uma mudança não está completa se apenas um consumidor consegue codificá-la.
Para alterações binárias, os mesmos vetores devem produzir e consumir os
mesmos bytes no ESP-IDF/C++, Arduino e Qt/desktop.

## Vetores e casos mínimos

Quando o suporte de testes for introduzido, vetores relacionados a framing ou
payload devem cobrir, quando aplicável:

- payload vazio e tamanhos mínimo e máximo;
- bytes `0x00`, `0x0A` e `0x0D` dentro do payload;
- valores de fronteira de cada largura inteira e de ponto flutuante;
- frame truncado, tamanho inconsistente e versão incompatível;
- CRC válido e inválido;
- fragmentação em todos os limites relevantes;
- request ID repetido para comprovar deduplicação de comandos;
- round trip e igualdade byte a byte entre plataformas.

CSV pode ser usado em testes e diagnóstico, mas não substitui vetores
`PACKED_LE` de produção.

## Testes obrigatórios

Toda contribuição ao código compartilhado deverá executar:

- testes unitários do codec e do framing;
- testes dos vetores canônicos;
- build com warnings tratados como erro nas plataformas suportadas;
- testes de integração dos consumidores afetados.

Execute ao menos:

```text
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
pio run -d tests/embedded
```

O `ctest` inclui a validação das descrições JSON, a igualdade byte a byte dos
`.bin`, o decode de todos os campos e os motivos exatos dos casos inválidos.
Consulte [`CONFORMANCE.md`](CONFORMANCE.md) antes de alterar qualquer
vetor. Arquivo `.bin` alterado é mudança de contrato e nunca uma fixture local
descartável.

Mudanças somente documentais também devem validar links relativos, a árvore
documentada e a correspondência entre decisões, arquitetura e versão. Uma
contribuição deve informar exatamente o que foi executado e qualquer teste que
não pôde ser realizado.

## Checklist de revisão

- [ ] O escopo respeita as dependências dos tópicos.
- [ ] Nenhum caminho legado foi introduzido.
- [ ] Especificação e ADRs refletem a implementação.
- [ ] O impacto SemVer foi classificado.
- [ ] Vetores e testes cobrem sucesso, erro e bytes arbitrários.
- [ ] Timestamps de origem e separação de canais foram preservados.
- [ ] Não há cópia independente do contrato em outro repositório.
- [ ] Decisões ainda abertas estão registradas, não presumidas pelo código.
