# Política de versionamento

## Unidade de release

O repositório publica uma linha de releases `vMAJOR.MINOR.PATCH` que identifica
em conjunto:

- a revisão da especificação BTP;
- a biblioteca compartilhada correspondente;
- os vetores de conformidade daquela revisão.

Enquanto o contrato estiver em elaboração, as versões são `0.y.z` e podem
mudar de forma incompatível. O primeiro wire format congelado será `v1.0.0`.

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

O wire format carregará versão suficiente para rejeitar incompatibilidades e
negociar extensões. A posição e a codificação desse campo serão definidas no
tópico de wire format.

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
de `bally_software`, `t_dongle_develop` ou `TraceView`. Atualizações são feitas
mudando a referência da dependência e executando os testes de conformidade.

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
