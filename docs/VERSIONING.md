# Política de versionamento

## Unidade de release

O repositório publica uma linha de releases `vMAJOR.MINOR.PATCH` que identifica
em conjunto:

- a revisão da especificação BTP;
- a biblioteca compartilhada correspondente;
- os vetores de conformidade daquela revisão.

Enquanto o contrato estiver em elaboração, as versões são `0.y.z` e podem
mudar de forma incompatível. O primeiro wire format publicado será `v1.0.0`.

## SemVer aplicado ao BTP

| Incremento | Quando usar |
| --- | --- |
| `MAJOR` | Mudança incompatível no wire, na interpretação de campos existentes ou nas garantias públicas da biblioteca. |
| `MINOR` | Extensão compatível e opcional, novo tipo/mensagem negociável ou nova API compatível da biblioteca. |
| `PATCH` | Correção compatível, melhoria interna ou esclarecimento editorial que não muda bytes nem semântica observável. |

Uma correção textual que altera a interpretação válida de bytes não é
editorial: exige a classificação compatível ou incompatível apropriada,
novos vetores e release.

## Identificação no wire e na biblioteca

O wire format carrega a versão no campo definido em
[`BTP_V1.md`](BTP_V1.md), suficiente para rejeitar incompatibilidades e
negociar extensões explicitamente.

A biblioteca deve expor sua versão e o intervalo de versões de protocolo que
suporta. A versão do artefato segue a release do repositório, mesmo quando uma
release altera apenas a documentação, testes ou implementação.

## Compatibilidade

- Implementações devem rejeitar versões major incompatíveis.
- Extensões de minor precisam de regra explícita de negociação ou de
  ignorabilidade segura; não se presume compatibilidade apenas pelo número.
- Não haverá modo legado, parser alternativo ou fallback silencioso.
- Uma implementação não pode emitir uma extensão que o peer não anunciou
  suportar, quando a extensão exigir negociação.

As regras detalhadas de handshake e sessão pertencem aos tópicos posteriores.

## Dependências dos consumidores

Cada repositório consumidor deve registrar a versão exata do BTP utilizada em
seu mecanismo de dependências e em seus artefatos de build. Durante
desenvolvimento coordenado, uma revisão Git imutável também pode ser fixada.

Não é permitido copiar a especificação ou o código da biblioteca para dentro
de `Bally_OS`, `Bally_dongle` ou `TraceView`. Atualizações são feitas
mudando a referência da dependência e executando os testes de conformidade.

## Branches de release

- `main` contém sempre a versão estável mais recente da linha MAJOR em
  desenvolvimento.
- Uma branch de manutenção só é criada ao avançar para uma nova versão
  MAJOR (`1.0` → `2.0`, `2.0` → `3.0`, ...): antes de `main` receber a
  primeira mudança incompatível da MAJOR seguinte, corta-se a branch `N.x`
  (ex.: `1.x`) a partir do último commit da linha MAJOR anterior.
- MINOR e PATCH dentro da mesma MAJOR não recebem branch própria; ficam
  identificados apenas por tag (`vMAJOR.MINOR.PATCH[-suffix]`) no histórico
  de `main` ou da branch de manutenção correspondente.
- Uma branch `N.x` recebe somente PATCH retrocompatível da sua própria
  linha (backport de correção). MINOR, MAJOR e features novas não são
  portadas para trás.
- Toda release, em qualquer branch, precisa da tag correspondente no commit
  exato (ver "Processo de release" abaixo); é essa tag que os repositórios
  consumidores fixam em `lib_deps`.

## Processo de release

Uma release somente pode ser identificada depois de:

1. atualizar especificação e decisões afetadas;
2. classificar o impacto SemVer;
3. adicionar ou atualizar vetores de conformidade;
4. passar os testes nas plataformas suportadas;
5. registrar impactos e migração no changelog da release.

Tags e pacotes não devem ser criados automaticamente como parte de uma
alteração de documentação ou implementação; publicação exige uma ação
explícita do mantenedor.
