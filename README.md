# Bally Telemetry Protocol (BTP)

O **Bally Telemetry Protocol (BTP)** é o contrato binário compartilhado entre
o firmware do robô, o dongle e as aplicações de computador do ecossistema
Bally. Este repositório é a fonte canônica da especificação, das decisões de
arquitetura e, nas próximas etapas, do codec e dos vetores de conformidade.

## Estado atual

O BTP v1 está em fase de especificação. A fundação e as responsabilidades dos
componentes estão documentadas, mas o wire format ainda **não está congelado**
e não existe codec pronto para uso. Os detalhes serão definidos de forma
sequencial pelos tópicos em [`topicos/`](topicos/).

Não existe nem será criado suporte ao protocolo legado. Até a publicação de
uma versão identificada, nenhum consumidor deve tratar o conteúdo atual como
um contrato estável.

## Projetos consumidores

| Projeto | Responsabilidade no BTP |
| --- | --- |
| `bally_software` (`bally_OS`) | Produzir telemetria e logs, executar comandos e originar timestamps no robô. |
| `t_dongle_develop` | Atuar como gateway entre ESP-NOW e USB Serial, rotear canais, manter catálogo e ações persistidas. |
| `TraceView` | Descobrir fontes e tópicos, apresentar telemetria e enviar intenções de comando; não definir a semântica dos comandos. |

Consulte [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) para os fluxos e os
limites de responsabilidade.

## Princípios do contrato

- O protocolo é binário, versionado e testável.
- Payloads são sequências opacas de bytes com tamanho explícito; não são
  strings e não usam `0x00`, CR ou LF como terminador.
- Valores no wire usam larguras fixas e serialização little-endian.
- Estruturas C/C++ nunca são transmitidas por `reinterpret_cast` ou cópia de
  memória bruta.
- `LOG`, `TELEMETRY`, `COMMAND` e `TERMINAL` são canais lógicos distintos.
- Telemetria identifica `source + topic + field`; um tópico nunca representa
  um gráfico específico.
- O timestamp nasce na origem e não é substituído pelo dongle.
- CRC detecta corrupção, mas não autentica origem nem conteúdo.

## Organização

```text
bally_protocol/
|-- README.md
|-- CONTRIBUTING.md
|-- PLANO_GERAL.txt
|-- docs/
|   |-- ARCHITECTURE.md
|   |-- VERSIONING.md
|   `-- decisions/
`-- topicos/
```

As decisões aceitas ficam em [`docs/decisions/`](docs/decisions/README.md).
A política de releases e compatibilidade está em
[`docs/VERSIONING.md`](docs/VERSIONING.md).

## Consumo e distribuição

Especificação e código compartilhado pertencem a este repositório. Os projetos
consumidores devem fixar uma versão publicada (tag, pacote ou revisão imutável)
e integrar o artefato a partir daqui. Não é permitido manter cópias
independentes da especificação, do codec ou de arquivos-fonte compartilhados
nos repositórios consumidores.

O mecanismo de empacotamento será definido quando a biblioteca compartilhada
for criada; essa escolha não altera a regra de fonte canônica única.

## Como contribuir

Leia [`CONTRIBUTING.md`](CONTRIBUTING.md). Qualquer mudança de wire format deve
ser acompanhada de decisão documentada, classificação de versão e vetores de
teste antes de ser aceita.
