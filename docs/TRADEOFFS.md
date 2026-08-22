# Vantagens, limites e aplicabilidade

Nada aqui é normativo. Este capítulo existe para responder a uma pergunta que os
capítulos de especificação não respondem: **por que escolher o BTP, e quando não
escolher.** Cada afirmação aponta para o documento que a define, e nenhum limite
citado aqui é estimativa — todos vêm de uma regra escrita.

Se você chegou para avaliar o protocolo antes de adotá-lo, este é o capítulo
para ler primeiro e o único que você pode ler isolado.

## 1. O problema

Um produtor embarcado gera duas classes de dado com exigências opostas —
amostras periódicas de sensor, que valem pouco individualmente e muito em série,
e operações de controle, que valem individualmente e não podem duplicar. Esses
dados precisam atravessar enlaces com características opostas: um rádio de banda
baixa, datagrama curto e perda alta, e um barramento local de banda maior,
orientado a byte-stream.

As soluções improvisadas para isso falham de maneiras previsíveis:

- **Texto sobre serial** (`printf` e parsing de linha) quebra no primeiro
  payload que contém `0x0A`, não distingue canal, e mistura diagnóstico com
  medição.
- **Transmitir a `struct`** funciona até o primeiro compilador, arquitetura ou
  flag de alinhamento diferente.
- **Timestamp na recepção** produz uma série temporal que mede a latência do
  transporte, não o fenômeno físico.
- **Um formato por enlace** duplica a semântica e faz cada gateway virar
  tradutor, com uma chance de reinterpretação por salto.

O BTP resolve esses quatro problemas de uma vez, e é isso que ele é: um envelope
binário de largura fixa, com identidade e instante criados na origem, que
atravessa transportes heterogêneos sem mudar de significado.

## 2. O que o desenho compra

### 2.1 O tempo pertence à origem

`source_id`, `boot_id`, `sequence` e `timestamp_us` são criados no produtor e
nenhum intermediário os reescreve — [`BTP_V1.md`](BTP_V1.md) §6 proíbe
explicitamente um gateway de substituir o timestamp pela hora de chegada.

A consequência prática é a que importa: a série temporal fica imune à latência
do rádio, à profundidade de fila do gateway e ao agendamento do consumidor. Um
gráfico plotado por `timestamp_us` mostra o fenômeno; plotado pela hora de
chegada, mostraria o transporte.

### 2.2 Um envelope, três transportes

O header de 36 octetos, o CRC e as invariantes de fragmentação são os mesmos em
ESP-NOW, serial e USB HID. Entre perfis muda **só a constante de limite**, não a
semântica — a mesma função de fragmentação é parametrizada pelo perfil e não
troca de comportamento.

Isso significa que adicionar um transporte é escrever um capítulo de perfil, não
uma segunda versão do protocolo.

### 2.3 Portabilidade sem ABI

Todos os inteiros multi-octeto são little-endian, inclusive o CRC; nenhum
componente transmite a representação de memória de uma `struct`; e
[`CONVENTIONS.md`](CONVENTIONS.md) proíbe derivar tamanho de `sizeof`,
alinhamento ou tipo implícito de enum. O `magic` é uma sequência de 4 octetos,
não um inteiro.

O resultado é que o mesmo frame é produzido e consumido de forma idêntica por um
MCU e por um desktop, em linguagens diferentes, sem negociar ABI.

### 2.4 Custa pouco para embarcar

A biblioteca compartilhada é C++11, **sem alocação** e sem dependência de
sistema operacional: o chamador fornece os buffers, os slots de reassembly e a
capacidade de armazenamento, e nada cresce dinamicamente. Ver
[`CODEC.md`](CODEC.md) e
[`STREAM_AND_REASSEMBLY.md`](STREAM_AND_REASSEMBLY.md).

O codec do envelope não depende de nenhuma biblioteca de cripto — a cifra vive
em um alvo separado (`btp::aead`), e desligá-la remove a dependência inteira.

### 2.5 Um gateway pode refragmentar mensagem cifrada sem a chave

Este é o ganho menos óbvio do desenho e vale destacar. O AAD da cifra é o header
da **mensagem lógica**, canonicalizado: `payload_size` é o tamanho do payload
cifrado completo, o bit `FRAGMENTED` entra limpo, e `fragment_index`/
`fragment_count` entram como `0` e `1`
([§8.3](BTP_V1.md#83-dados-associados-aad)).

Como os campos de fragmentação são excluídos do cálculo, o tag é o mesmo
independentemente de como a mensagem foi cortada. Um gateway pode então
remontar uma mensagem cifrada que chegou de um transporte e refragmentá-la para
outro **sem possuir a chave** e sem invalidar o tag. Sem essa canonicalização,
cifra e travessia de transportes seriam mutuamente exclusivas.

### 2.6 Conformidade é verificável, não interpretável

O contrato não é só prosa: [`CONFORMANCE.md`](CONFORMANCE.md) define vetores
binários canônicos, cada um com um `.json` legível e um `.bin` cru, mais
mutações inválidas com o motivo exato de rejeição de cada uma. Uma
implementação não está pronta quando o autor acha que entendeu o texto; está
pronta quando produz e consome os mesmos octetos dos vetores.

Alterar um vetor é declaradamente alterar o contrato, com todo o processo de
versão que isso implica.

### 2.7 Comando não espera telemetria

Os canais lógicos têm ordem de prioridade definida em
[`COMMANDS_AND_ACTIONS.md`](COMMANDS_AND_ACTIONS.md) §12, com filas separadas e
FIFO dentro de cada classe. Sob pressão, descarta-se telemetria primeiro, depois
log e status periódico, e a perda é contabilizada. Um emissor precisa reservar
capacidade para pelo menos uma mensagem das duas classes mais altas.

Em outras palavras: uma rajada de telemetria não pode atrasar um resultado de
comando, e isso é regra, não recomendação.

### 2.8 Falha determinística

Campo reservado é zero, e recebido diferente de zero **causa rejeição** — valor
não atribuído nunca é ignorado. Não há modo legado, autodetecção nem parser
alternativo. Frame com CRC divergente, tag inválido ou invariante violada é
descartado antes do roteamento, sem NACK
([`CONVENTIONS.md`](CONVENTIONS.md)).

Isso troca tolerância por diagnosticabilidade: um peer incompatível falha de
imediato e de forma legível, em vez de funcionar parcialmente por meses.

## 3. O que o desenho custa

Nenhum item abaixo é defeito de implementação — todos são consequências
assumidas, e vários estão registrados como não-objetivos explícitos.

### 3.1 O modelo de segurança é deliberadamente estreito

A matriz completa está em [`CRYPTO.md`](CRYPTO.md); o resumo do que **não**
está coberto:

| Não coberto | Consequência |
| --- | --- |
| Anti-replay | Um frame válido capturado pode ser reinjetado. Não há requisito escrito hoje, e a mitigação foi deixada para uma revisão futura. |
| Confidencialidade de metadados | Só o payload é cifrado. `source_id`, `boot_id`, `sequence`, `timestamp_us`, `type` e `object_id` viajam em claro mesmo com `ENCRYPTED`. |
| Rotação de chave, forward secrecy | Não existem. Comprometer a chave compromete o histórico capturado. |
| Identidade por peer | A autenticação é por posse da chave compartilhada. Dois peers com a mesma chave são indistinguíveis para a cifra. |

Se o seu ambiente exige qualquer uma dessas quatro coisas, o wire v2 não as
oferece e você precisará de uma camada externa.

### 3.2 Provisionamento fica fora do wire

Distribuir `source_id` e chave é explicitamente fora de escopo, e a chave
**MUST NOT** viajar em campo nenhum. As duas cifras usam tamanhos de chave
diferentes e não intercambiáveis: 16 octetos para AES-128-GCM, 32 para
ChaCha20-Poly1305.

Consequência operacional: antes de qualquer deploy você precisa de um mecanismo
próprio de provisionamento, e o protocolo não ajuda a construí-lo.

### 3.3 A cifra não se negocia em runtime

Marcar `ENCRYPTED` é decisão estática de configuração, fora de banda — não há
sinalização, descoberta ou negociação no `HELLO` ou em qualquer outro handshake
(§8.6). O sub-campo `CIPHER_ID` identifica no wire qual cifra produziu o
payload, mas não negocia nada.

Divergência de configuração entre dois endpoints é, por definição, erro de
deploy: não existe caso de wire para "um lado cifra e o outro não", e não há
fallback para claro dentro do mesmo canal.

### 3.4 Tetos de tamanho, e um deles é apertado

| Perfil | Frame | Payload por frame | Payload lógico máximo |
| --- | ---: | ---: | ---: |
| Serial (COBS) | 4096 | 4056 | limitado pela negociação |
| ESP-NOW | 250 | 210 | 53550 (`255 × 210`) |
| USB HID | 62 | 22 | 5610 (`255 × 22`) |

O teto de 255 fragmentos é do próprio campo `fragment_count`, que tem um octeto.
Acima dele não existe fragmentação alternativa: manifesto grande e resposta de
comando usam a mesma fragmentação comum, ou não passam.

O perfil USB HID é o caso apertado: com 22 octetos de payload, **até um `HELLO`
fragmenta** — comportamento normal do perfil, não falha de negociação. E é por
isso que AEAD está fora de escopo nele: um tag de 16 octetos sobre 22 de payload
é 73% de overhead, contra ~7,6% no ESP-NOW e ~0,4% na serial. Um encoder
**MUST NOT** marcar `ENCRYPTED` em frame destinado ao HID.

### 3.5 Um boot tem um número finito de mensagens

`sequence` identifica a mensagem lógica, tem 32 bits e **MUST NOT** dar wrap
dentro de um boot (§6). Isso é o que torna a tripla
(`source_id`, `boot_id`, `sequence`) uma identidade confiável — e o que impõe o
teto: esgotar a sequência exige um novo `boot_id`, não um wrap silencioso.

Para telemetria a 50 Hz o teto é distante; para um produtor de alta taxa que não
reinicia, é um número a calcular antes, não depois.

### 3.6 Telemetria é best-effort por projeto

Não há ACK por amostra e uma amostra perdida **nunca** é retransmitida. Fila
cheia descarta telemetria, preferindo a amostra mais recente, e contabiliza a
perda. A confiabilidade fim a fim existe só onde o tipo lógico a define — um
`COMMAND_REQUEST` é confirmado por `COMMAND_RESULT`, não pelo transporte.

Se você precisa de entrega garantida de cada amostra, o BTP não é o lugar de
obtê-la, e o `TELEMETRY` não deve ser usado como se fosse.

### 3.7 O consumidor carrega obrigações

Como não há alocação, o dimensionamento é seu: quantidade de slots de
reassembly, capacidade de armazenamento por slot, profundidade de fila por
classe de prioridade. Um estouro é uma rejeição contabilizada, não um `realloc`.

E há uma obrigação fácil de esquecer: uma mensagem completa **continua ocupando
o slot** para manter a `ByteView` estável, até o consumidor chamar
`release()`. Slots completos também expiram se não forem liberados.

### 3.8 Migração é coordenada, não incremental

Não haverá modo legado, parser alternativo nem fallback silencioso — a
[ADR 0001](decisions/0001-fonte-canonica-e-sem-legado.md) fecha essa porta de
propósito, e a política de versão repete a regra.

Isto é vantagem e custo ao mesmo tempo, e vale dizer as duas metades. A
vantagem: nenhuma dívida de compatibilidade, nenhum caminho antigo para
sustentar, comportamento único e testável. O custo: uma mudança incompatível
exige atualizar os consumidores de forma coordenada, e um deles atrasado
bloqueia o conjunto. Uma mudança no wire só está completa quando **todas** as
plataformas suportadas produzem e consomem os mesmos octetos.

## 4. Onde o BTP cabe

O protocolo foi desenhado para este formato de problema, e é onde ele rende mais:

- **Telemetria periódica com controle no mesmo canal**, de um produtor embarcado
  para um ou poucos consumidores conhecidos.
- **Enlaces heterogêneos em série** — tipicamente um rádio curto seguido de um
  barramento local — onde reenquadrar sem reinterpretar é requisito.
- **Séries temporais que precisam do instante da origem**, não da chegada.
- **Ambientes onde a conformidade precisa ser auditável**: múltiplas
  implementações, múltiplas linguagens, e a necessidade de provar equivalência
  em octetos.
- **Alvos com restrição de memória**, onde alocação dinâmica é indesejada ou
  proibida.
- **Confiança na rede física ou perímetro controlado**, com ou sem cifra de
  payload conforme o caso.

## 5. Onde o BTP não cabe

Igualmente importante, e o motivo de cada um:

- **Rede multi-hop com roteamento dinâmico.** Topologia, descoberta de rota e
  endereçamento de rede estão fora de escopo. O protocolo pressupõe que você
  sabe para onde o frame vai.
- **Ambiente hostil que exija anti-replay ou identidade forte por peer.** Ver
  3.1 — as duas coisas não existem nesta versão.
- **Muitos peers com chaves distintas.** Não há gerência de chave, rotação nem
  identidade criptográfica por origem.
- **Transferência de arquivo ou streaming de payload grande.** O teto de 255
  fragmentos e a ausência de retransmissão fazem disso o trabalho errado para
  este protocolo.
- **Evolução incremental sem coordenar consumidores.** Ver 3.8. Se você não
  controla os dois lados, a ausência de modo legado é um custo alto.
- **Descoberta de serviço em rede aberta.** O manifesto descreve o catálogo de
  um produtor conhecido; não é um mecanismo de descoberta de rede.

## 6. Resumo em uma frase

O BTP troca **flexibilidade de rede e tolerância a peer divergente** por
**determinismo, portabilidade e conformidade verificável**, em um envelope
pequeno o suficiente para caber num datagrama de rádio. Se o seu problema é
mover medição e controle de um embarcado para um computador através de enlaces
diferentes, sem perder a origem do tempo, essa troca é favorável. Se o seu
problema é rede, não é.

## Para continuar

- Como o protocolo funciona na prática, passo a passo:
  [Do sensor à tela](WALKTHROUGH.md).
- O modelo de papéis e as garantias por canal:
  [O protocolo: modelo e garantias](ARCHITECTURE.md).
- O contrato normativo, octeto a octeto: [O frame no wire](BTP_V1.md).
- Como as palavras **MUST** e **SHOULD** devem ser lidas:
  [Convenções e glossário](CONVENTIONS.md).
