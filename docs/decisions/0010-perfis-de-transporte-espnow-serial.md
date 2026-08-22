# ADR 0010: perfis de transporte ESP-NOW e Serial/COBS

- Estado: Aceita
- Data: 2026-08-09
- Detalha: [ADR 0005](0005-transportes-entrega-e-integridade.md)

## Contexto

O ADR 0005 escolheu COBS para a serial, separou o console humano e tornou
telemetria best effort, mas deixou para a especificação posterior os limites,
o framing exato, a ressincronização e a distinção entre aceite local, entrega
no enlace e conclusão de uma operação.

Os dois firmwares atuais limitam seus pacotes ESP-NOW a 250 octetos. A serial
é um stream compartilhado por console, logs e protocolo e, sem propriedade
única, escritas concorrentes podem corromper frames binários. A baud nominal
também não prova a capacidade sustentada do enlace real.

## Decisão

- Cada datagrama ESP-NOW carrega exatamente um frame ou fragmento BTP, sem
  wrapper ou padding, limitado a 250 octetos.
- O retorno `ESP_OK` de `esp_now_send` registra somente aceite local. O
  callback registra confirmação do enlace, não decode ou execução remota.
- Comandos usam seu resultado BTP como confirmação fim a fim e podem ter
  retry limitado com identidade e bytes preservados. Telemetria não tem ACK
  por amostra nem retry.
- O ESP-NOW não garante ordem, unicidade ou entrega; reassembly aceita
  fragmentos fora de ordem e nunca entrega conteúdo parcial.
- Cada pacote serial é exatamente `0x00 || COBS(frame BTP) || 0x00`.
- O frame serial decodificado é limitado a 4096 octetos, o bloco COBS a 4113
  e o pacote com delimitadores a 4115.
- Overflow ou candidato inválido é descartado até o próximo delimitador, que
  restabelece a sincronização.
- O console inicia explicitamente o modo protocolado; somente um `SerialMux`
  escreve na porta enquanto esse modo estiver ativo. Watchdog ou fechamento
  explícito devolve a propriedade ao console.
- Baud permanece configuração fora do wire BTP, pois precisa ser conhecida
  antes do handshake. O valor inicializado é anunciado a partir da mesma fonte
  de configuração, e a capacidade sustentada é medida no hardware liberado.

As regras normativas completas estão em
[`TRANSPORT_ESPNOW.md`](../TRANSPORT_ESPNOW.md) e
[`TRANSPORT_SERIAL.md`](../TRANSPORT_SERIAL.md).

## Consequências

- O limite conservador de 250 octetos funciona nos dois firmwares atuais;
  datagramas longos de versões mais novas do ESP-NOW exigem negociação ou
  nova versão do contrato.
- Sucesso do rádio não pode ser apresentado ao usuário como sucesso de um
  comando.
- Perda de telemetria libera capacidade em vez de criar tempestades de retry.
- O overhead serial máximo fica conhecido e a memória do decoder é limitada.
- Delimitadores redundantes permitem recuperar o próximo frame após ruído,
  truncamento ou overflow.
- Logs humanos precisam virar frames `LOG`, ser retidos ou ser descartados
  durante uma sessão; não podem escrever diretamente na serial.
- A taxa de telemetria suportada depende de medição do build e da placa, não
  apenas do número configurado como baud.

## Alternativas consideradas

- **Usar o maior datagrama oferecido pelo ESP-NOW v2:** rejeitado porque não é
  o limite comum garantido pelos firmwares e peers atuais.
- **Tratar callback ESP-NOW como ACK do comando:** rejeitado porque o callback
  não confirma decode, roteamento ou execução.
- **Retransmitir toda telemetria perdida:** rejeitado porque aumenta o
  congestionamento e pode bloquear comandos prioritários.
- **Usar apenas um `0x00` entre frames seriais:** rejeitado para emissores; o
  delimitador inicial explícito facilita iniciar e recuperar o parser. O
  receptor ainda ignora delimitadores vazios com segurança.
- **Permitir que cada produtor escreva diretamente na serial:** rejeitado
  porque chamadas concorrentes podem intercalar texto e bytes COBS.
- **Fixar 5 Mbaud no protocolo:** rejeitado porque build, driver e USB CDC
  podem ter comportamento diferente e a taxa nominal não mede goodput.

## Impacto de versão

Esta decisão completa o contrato do wire v1 e integrou a primeira release da
linha `1`; não havia release anterior a incrementar. Depois da publicação,
mudança incompatível no framing, nos limites ou nas garantias de entrega exige
incremento `MAJOR`.
