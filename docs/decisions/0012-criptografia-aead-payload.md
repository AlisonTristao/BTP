# ADR 0012: criptografia AEAD do payload (wire `0x02`)

- Estado: Proposta
- Data: 2026-08-21
- Detalha: [ADR 0005](0005-transportes-entrega-e-integridade.md), [ADR 0007](0007-wire-format-btp-v1.md)

## Contexto

O CRC32 do envelope detecta corrupção acidental, mas explicitamente **não
fornece autenticação** (`BTP_V1.md` §7, ADR 0005). O salto ESP-NOW
(produtor↔gateway) é rádio aberto: qualquer um no alcance pode capturar ou
injetar tráfego passivamente, sem precisar de acesso físico a nada. Isso é
diferente dos perfis USB (`Serial`/`UsbHid`, ADR 0010/0011), que exigem posse
física do dispositivo para serem interceptados — por isso esta ADR não altera a
política desses dois perfis.

O perfil `UsbHid` tem só 22 octetos de payload por fragmento
(`BTP_USB_HID_MAX_PAYLOAD_SIZE`, ADR 0011); um tag de autenticação de 16
octetos consome 73% desse orçamento e derruba o limiar de mensagem
não-fragmentada de 22 para 6 octetos de dado real. A política de
criptografia para `UsbHid` (provavelmente tag truncado) fica **deliberadamente
fora do escopo desta ADR** e será tratada em revisão futura; por ora,
`ENCRYPTED` não é habilitada nesse perfil.

## Decisão

1. Novo bit em `flags` (`BTP_V1.md` §5, hoje `0xFFFE` reservado):
   `ENCRYPTED = 0x0002`. Quando marcado, o payload lógico é
   `ciphertext ‖ tag` em vez de dado em claro.

2. **Algoritmo primário: AES-128-GCM.**
   - Os endpoints embarcados de referência usam um MCU com acelerador AES em
     hardware, e o lado desktop usa CPU x86_64 com AES-NI. Nos dois casos GCM
     é hardware-acelerado via mbedtls — já embutido nos SDKs usados, sem
     dependência nova.
   - Em qualquer chip **sem** acelerador AES, GCM cai para software: mais
     lento e, se a implementação não for constant-time, sujeito a
     side-channel de timing. Um endpoint futuro nessas condições **SHOULD**
     usar ChaCha20-Poly1305 em vez de AES-128-GCM — mesmo tamanho de nonce e
     tag, constant-time por construção, sem depender de hardware.
   - **A escolha entre as duas cifras é sinalizada no wire por `CIPHER_ID`,
     um sub-campo de 2 bits em `flags`** (bits 2 e 3, máscara `0x000C`,
     deslocamento 2 — `BTP_V1.md` §5/§8.1): `0` para AES-128-GCM (padrão),
     `1` para ChaCha20-Poly1305, `2`/`3` reservados para cifras futuras e
     **MUST** ser rejeitados por um decoder desta versão. Com `ENCRYPTED`
     limpo, `CIPHER_ID` **MUST** ser `0` — não há cifra "em uso" por um
     frame não cifrado — e um valor diferente de zero nesse caso **MUST**
     também ser rejeitado. Isso não é negociação em tempo de execução: os
     dois endpoints de um canal ainda **MUST** estar configurados fora de
     banda para suportar e aceitar as cifras que usam entre si (ver
     "Estratégia de negociação" abaixo); o que muda é que a identificação de
     qual cifra produziu cada frame deixa de depender só dessa configuração
     externa e passa a ser lida diretamente do próprio frame.
   - `CIPHER_ID` é um sub-campo de 2 bits — um valor de 1-em-4 — em vez de
     duas flags booleanas independentes (`AES_GCM`/`CHACHA20_POLY1305`)
     porque duas flags abririam combinações que o wire não deveria conseguir
     representar: as duas marcadas ao mesmo tempo (qual cifra vale?) ou,
     pior, nenhuma marcada com `ENCRYPTED` ligado (qual cifra foi usada?). Um
     sub-campo de 2 bits tira essa ambiguidade por construção — sobram
     exatamente 4 valores possíveis, dois atribuídos e dois reservados —,
     reaproveitando o mesmo princípio já usado para bits reservados do resto
     de `flags`: um valor não atribuído é rejeitado explicitamente, nunca
     ignorado.
   - **As duas cifras usam tamanhos de chave diferentes, não intercambiáveis:**
     16 octetos (128 bits) para AES-128-GCM, 32 octetos (256 bits) para
     ChaCha20-Poly1305 — a RFC 8439 exige 256 bits para ChaCha20-Poly1305,
     sem variante padronizada de 128 bits. `CIPHER_ID` sinaliza, portanto,
     não só qual cifra mas também qual tamanho de chave um frame usa; o
     provisionamento fora de banda (item 6 abaixo) **MUST** gerar e
     armazenar, para cada canal, a chave no tamanho correspondente à cifra
     configurada para esse canal.

3. **Nonce = `source_id (4B) ‖ boot_id (4B) ‖ sequence (4B)`** — exatamente os
   96 bits que GCM/ChaCha20-Poly1305 exigem, sem nenhum octeto novo no
   header:
   - `source_id` já **MUST** ser não-zero e único no domínio de roteamento
     (`BTP_V1.md` §6).
   - `boot_id` já **MUST** ser escolhido de novo a cada boot e nunca reusado
     enquanto frames do boot anterior possam existir (`BTP_V1.md` §6). É essa
     regra, já normativa, que impede o cenário catastrófico para GCM: sem
     `boot_id` no nonce, um reboot de qualquer dos endpoints reiniciaria
     `sequence` do zero e colidiria com nonces já usados sob a mesma chave em
     sessões anteriores (reuso de keystream, tag forjável). Com `boot_id`
     presente, cada boot ocupa uma faixa de nonce disjunta das anteriores.
   - `sequence` identifica a mensagem lógica e **MUST NOT** se repetir no
     mesmo boot (`BTP_V1.md` §6), dando a unicidade por mensagem dentro de um
     boot.
   - A combinação é única por construção das regras que o protocolo já
     exige — não precisa de contador de nonce dedicado nem de campo novo.

4. **AAD (dados associados) = os 36 octetos do header.** Continuam em claro
   — necessário para o pipeline de validação existente rotear por `type`,
   `object_id` etc. antes de decifrar (`BTP_V1.md` §8) — mas qualquer bit
   alterado no header invalida o tag tanto quanto alterar o payload.
   - **O header do AAD é o da mensagem lógica, com os campos de fragmentação
     canonicalizados:** `payload_size` é o do `ciphertext ‖ tag` completo, o
     bit `FRAGMENTED` entra limpo e `fragment_index`/`fragment_count` entram
     como `0`/`1` (`BTP_V1.md` §8.3). Sem essa regra, o AAD ficava
     indefinido para mensagem fragmentada: o tag é calculado uma vez, antes
     de fragmentar (item 5), mas cada fragmento carrega no wire um
     `payload_size` e um `fragment_index` diferentes, então não existe "o
     header" do frame para usar — e dois endpoints que escolhessem
     convenções diferentes falhariam **todo** tag de mensagem fragmentada,
     de forma indistinguível de chave errada ou de ataque.
   - Canonicalizar em vez de fixar uma convenção qualquer (por exemplo
     "índice 0 e a contagem real") tem uma segunda razão, específica desta
     topologia: um gateway roteia entre perfis com limites de payload
     diferentes (210 octetos no `EspNow`, 22 no `UsbHid` — ADR 0010/0011).
     Com os campos de fragmentação fora do AAD, ele **pode** reassemblar e
     refragmentar uma mensagem cifrada para o outro enlace sem possuir a
     chave. Com eles dentro, refragmentar invalidaria o tag, e o gateway
     precisaria da chave só para reenquadrar — o que contraria o papel dele
     (ADR 0004: roteia e retransmite, não reinterpreta).

5. **O tag (16 octetos) é calculado e verificado no nível da mensagem
   lógica**, antes de fragmentar (emissor) / depois de reassemblar
   (receptor) — nunca por fragmento. `sequence` já identifica a mensagem
   lógica, não o frame físico (`BTP_V1.md` §6), então isso evita ter que
   derivar um nonce por fragmento (que exigiria incluir `fragment_index` no
   nonce e abriria mais uma forma de errar). O payload lógico cifrado vira
   `ciphertext ‖ tag` e é fragmentado pelas regras já existentes, sem
   mudança de comportamento na fragmentação em si.

6. **A chave nunca trafega no wire, em nenhum campo.** É provisionada fora de
   banda — por par de endpoints ou por rede — e vive em armazenamento não
   volátil nos dois lados do rádio e em configuração local no consumidor.
   `boot_id` **não deriva nem substitui** a chave: são conceitos
   independentes. A chave é o segredo estático de longo prazo; `boot_id` só
   contribui para a unicidade pública do nonce, exatamente como qualquer
   outro campo do header — pode ser lido por qualquer um, não é secreto por
   si só.

## Consequências

- Payload lógico cresce exatamente 16 octetos por **mensagem** (o tag), não
  por fragmento; o ciphertext em si tem o mesmo tamanho do plaintext (GCM é
  modo de fluxo, sem padding).
- No perfil `EspNow` (210 octetos de payload), isso é ~7,6% de overhead;
  pode empurrar mensagens perto do teto para 1 fragmento a mais, mas não
  muda o comportamento de fragmentação em si.
- `ENCRYPTED` desligada permanece byte a byte idêntica ao comportamento
  atual — extensão opcional, não quebra tráfego em claro já publicado.
- Tag inválido segue a mesma política já existente para CRC divergente:
  descarte antes de entregar ao consumidor, sem notificação ativa ao emissor
  — preserva a decisão de não ter ACK/NACK no protocolo (ADR 0005).
- Exige mecanismo de provisionamento de chave por par/rede — fora do wire
  format em si, mas obrigatório operacionalmente antes de qualquer release.
- `UsbHid` permanece sem `ENCRYPTED` até uma ADR futura decidir sua política
  de tag (provável truncamento).

## Alternativas consideradas

- **ChaCha20-Poly1305 como cifra única e padrão:** rejeitado como primário
  porque abriria mão da aceleração de hardware disponível hoje nos dois
  endpoints ESP32-S3 do projeto; mantido como alternativa declarada por
  endpoint para chips sem acelerador AES.
- **Truncar o tag para 8 octetos por padrão, em todos os perfis:** rejeitado
  como escolha global — reduziria a resistência a forjadura de 2⁻¹²⁸ para
  2⁻⁶⁴ por tentativa sem necessidade, já que `EspNow` e `Serial` têm espaço
  de sobra (7,6% e 0,4% de overhead respectivamente). Fica reservado como
  mitigação pontual, específica de `UsbHid`, para revisão futura.
- **Substituir o CRC32 pelo tag AEAD:** rejeitado — o CRC continua útil como
  descarte barato de lixo de transporte por fragmento, antes de gastar
  ciclos de cripto na mensagem completa reassemblada. As duas camadas têm
  papéis diferentes: framing vs. autenticidade.
- **Incluir a chave, ou parte dela, em algum campo do header:** rejeitado —
  inverteria o propósito da criptografia; um nonce pode e deve ser público,
  a chave nunca.
- **Incluir os campos de fragmentação no AAD** (o header do fragmento 0, ou o
  header lógico com a contagem real de fragmentos): rejeitado. Autenticar
  `fragment_index`/`fragment_count` não protege nada que o tag da mensagem
  reassemblada já não proteja — um fragmento alterado, perdido, duplicado ou
  reordenado produz um `ciphertext ‖ tag` diferente e cai na verificação do
  tag de qualquer forma. Em troca, amarraria o tag ao enquadramento de um
  transporte específico: a mesma mensagem lógica passaria a ter tags
  diferentes conforme o enlace por onde saiu, o gateway perderia a
  possibilidade de refragmentar sem a chave, e o emissor teria que conhecer o
  limite do transporte de destino antes de cifrar.
- **Duas flags booleanas independentes, uma por cifra, em vez do sub-campo
  `CIPHER_ID` de 2 bits:** rejeitado — permitiria combinações inválidas que
  um decoder teria que detectar e rejeitar à parte (as duas marcadas ao
  mesmo tempo, ou, com `ENCRYPTED` ligado, nenhuma marcada). Um sub-campo de
  2 bits — um valor de 1-em-4 — torna essas combinações irrepresentáveis por
  construção: só existem 4 valores possíveis, dois atribuídos (`0`/`1`) e
  dois reservados (`2`/`3`), sem espaço para "as duas ao mesmo tempo".

## Plano de implementação

Segue o processo obrigatório de `CONTRIBUTING.md` § "Processo para mudar o
wire format" para mudanças de bytes/interpretação/garantias do wire:

1. **Motivação e impacto nas implementações consumidoras** — coberto nesta
   ADR; os endpoints embarcados ganham cripto acelerada por hardware e o lado
   desktop usa AES-NI via mbedtls/OpenSSL. Nenhum deles perde compatibilidade
   com tráfego não cifrado.
2. **ADR** — este documento (0012). Passa para `Aceita` quando os itens
   abaixo estiverem implementados e testados.
3. **Atualização da especificação canônica** (`BTP_V1.md`) — pendente: nova
   seção normativa de criptografia AEAD, entrada `ENCRYPTED` na tabela de
   flags, atualização de `version` do wire para `0x02`.
4. **Classificação SemVer** — `MAJOR`: decisores acordaram tratar
   como incompatível porque um decoder de wire v1 **MUST** rejeitar qualquer frame
   com bit reservado marcado (`BTP_V1.md` §5), logo não é seguramente
   ignorável por um peer antigo mesmo sendo opcional em uso.
5. **Vetores de conformidade** — pendente: casos válidos (round-trip
   cifrado) e inválidos (tag corrompido, AAD alterado, payload truncado
   antes do tag) em um novo diretório `test-vectors/v2/`.
6. **Implementação equivalente nas plataformas afetadas** — pendente:
   `btp::codec`/`btp::fragmentation` (biblioteca compartilhada), wiring de
   `mbedtls_gcm_*` nos endpoints embarcados e na aplicação consumidora.
7. **Estratégia de negociação** — pendente: confirmar se `ENCRYPTED` é
   decisão estática de configuração por par (endpoints já provisionados
   com a mesma chave, sem negociação em runtime) ou se precisa de sinal
   explícito no handshake `HELLO` existente.
8. **Notas de migração** — a linha do wire v1 continua servida pela branch `1.x`
   (cortada do último commit da linha `1`, `d736c50`, antes desta ADR). O wire
   v2 não terá modo legado, parser alternativo nem fallback para payload em
   claro dentro do mesmo canal, por princípio já registrado em
   `CONTRIBUTING.md`.

## Impacto de versão

Classificado como `MAJOR` (`docs/VERSIONING.md`). Branch de
manutenção `1.x` cortada do commit `d736c50` antes desta mudança começar a
avançar em `main`, conforme "Branches de release".
