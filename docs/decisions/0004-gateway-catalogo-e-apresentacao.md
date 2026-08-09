# ADR 0004: gateway, catálogo e apresentação

- Estado: Aceita
- Data: 2026-08-09

## Contexto

Ações precisam sobreviver à desconexão do computador e ser descobertas por
clientes diferentes. Duplicar a semântica dessas ações no TraceView criaria
configurações incompatíveis.

## Decisão

O dongle é gateway de transporte, apresenta o catálogo ao computador e é dono
das ações virtuais persistidas. Ele roteia o contrato canônico entre os
enlaces, preservando origem e timestamp.

O TraceView é uma camada de apresentação. Ele descobre tópicos e ações, associa
gráficos a `source + topic + field`, coleta parâmetros do usuário e envia
intenções. Não define localmente o significado de um comando.

## Consequências

- Um computador novo pode reconstruir a interface a partir do manifesto e do
  catálogo.
- Layout e aparência continuam locais ao cliente; identidade e semântica de
  dados e ações não.
- Formatos de manifesto e ações persistidas serão definidos em tópicos
  posteriores.
