# ADR 0009: comandos, descoberta, sessão e terminal

- Estado: Aceita
- Data: 2026-08-09
- Impacto SemVer: baseline do wire v1, anterior à primeira release publicada (`v1.1.0-beta`)

## Contexto

Clientes precisam descobrir fontes, tópicos e ações sem configuração
duplicada. Comandos retransmitidos não podem repetir efeitos, respostas
precisam de correlação globalmente inequívoca e o terminal deve transportar
bytes sem confundi-los com telemetria ou com o console humano.

## Decisão

Os payloads de `COMMAND`, `CONTROL` e `TERMINAL` seguem os layouts binários de
[`../COMMANDS_AND_ACTIONS.md`](../COMMANDS_AND_ACTIONS.md). A identidade do
request é a tripla (`source_id`, `boot_id`, `sequence`); resultados carregam a
tripla em `reply_to`, e executores reservam essa chave antes de qualquer efeito
e não a expulsam durante o boot ativo do solicitante.

`HELLO` negocia a versão e limites da sessão. Manifestos versionados descrevem
uma fonte, seus tópicos, fields e ações. Assinaturas distinguem taxa solicitada
de taxa efetiva. Status publica contadores monotônicos de frames, perdas, CRC e
reassembly. Terminal transporta somente bytes opacos em objetos de entrada e
saída próprios.

A serial entra no modo protocolado por uma linha explícita com nonce e sai por
mensagem BTP confirmada ou watchdog; o modo binário nunca procura escapes
textuais. Comandos e controle de sessão têm prioridade sobre tráfego de volume.

## Consequências

- Retransmissão válida devolve o resultado original sem nova execução.
- Reinício do executor torna o `target_boot_id` antigo inválido.
- Um computador novo pode reconstruir catálogo e formulários de ação pelo
  manifesto, enquanto aparência permanece local.
- Manifestos e resultados grandes reutilizam a fragmentação do envelope.
- Sessão e filas exigem limites explícitos, cache de deduplicação e métricas.
- Terminal não interpreta telemetria, logs ou delimitadores de linha.

## Alternativas consideradas

- **Usar apenas `sequence` na resposta:** rejeitado porque sequência só é
  única no par (`source_id`, `boot_id`).
- **Deduplicar somente ações declaradas idempotentes:** rejeitado porque retry
  de uma ação com efeito continuaria inseguro.
- **Manifesto em JSON:** rejeitado pelo tamanho e pelo custo de parsing nas
  plataformas embarcadas; descritores binários têm limites determinísticos.
- **Escape textual dentro do modo BTP:** rejeitado porque bytes opacos e frames
  codificados podem conter a mesma sequência.
- **Misturar terminal e telemetria:** rejeitado porque os consumidores e as
  garantias dos canais são distintos.
