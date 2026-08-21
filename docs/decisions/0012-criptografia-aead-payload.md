# ADR 0012: Criptografia AEAD do payload (BTP v2.0)

- Estado: Proposta
- Data: 2026-08-21
- Detalha: [ADR 0005](0005-transportes-entrega-e-integridade.md), [ADR 0007](0007-wire-format-btp-v1.md)

## Contexto

O CRC32 do envelope detecta corrupção acidental, mas explicitamente **não
fornece autenticação** (`BTP_V1.md` §7, ADR 0005). O salto ESP-NOW
(dongle↔robô) é rádio aberto: qualquer um no alcance pode capturar ou
injetar tráfego passivamente, sem precisar de acesso físico a nada. Isso é
diferente dos perfis USB (`Serial`/`UsbHid`, ADR 0010/0011), que exigem posse
física do dongle para serem interceptados — por isso esta ADR não altera a
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
   - `bally_dongle` e `bally_OS` rodam no mesmo chip, `esp32-s3-devkitc-1`
     (`platformio.ini` de ambos), que tem acelerador AES em hardware; o
     TraceView (Qt/C++, x86_64) tem AES-NI. Nos três consumidores atuais do
     protocolo, GCM é hardware-acelerado via mbedtls — já embutido no core
     `arduino-esp32`/ESP-IDF, sem dependência nova.
   - Em qualquer chip **sem** acelerador AES, GCM cai para software: mais
     lento e, se a implementação não for constant-time, sujeito a
     side-channel de timing. Um endpoint futuro nessas condições **SHOULD**
     usar ChaCha20-Poly1305 em vez de AES-128-GCM — mesmo tamanho de nonce e
     tag, constant-time por construção, sem depender de hardware. A escolha
     de cifra é uma decisão local de cada endpoint, não um campo no wire (ver
     "Estratégia de negociação" no plano abaixo).

3. **Nonce = `source_id (4B) ‖ boot_id (4B) ‖ sequence (4B)`** — exatamente os
   96 bits que GCM/ChaCha20-Poly1305 exigem, sem nenhum octeto novo no
   header:
   - `source_id` já **MUST** ser não-zero e único no domínio de roteamento
     (`BTP_V1.md` §6).
   - `boot_id` já **MUST** ser escolhido de novo a cada boot e nunca reusado
     enquanto frames do boot anterior possam existir (`BTP_V1.md` §6). É essa
     regra, já normativa, que impede o cenário catastrófico para GCM: sem
     `boot_id` no nonce, um reboot do dongle ou do robô reiniciaria
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

5. **O tag (16 octetos) é calculado e verificado no nível da mensagem
   lógica**, antes de fragmentar (emissor) / depois de reassemblar
   (receptor) — nunca por fragmento. `sequence` já identifica a mensagem
   lógica, não o frame físico (`BTP_V1.md` §6), então isso evita ter que
   derivar um nonce por fragmento (que exigiria incluir `fragment_index` no
   nonce e abriria mais uma forma de errar). O payload lógico cifrado vira
   `ciphertext ‖ tag` e é fragmentado pelas regras já existentes, sem
   mudança de comportamento na fragmentação em si.

6. **A chave nunca trafega no wire, em nenhum campo.** É provisionada fora de
   banda — por par (dongle, robô) ou por rede — e vive em NVS/flash do
   ESP32-S3 nos dois lados e em configuração local no TraceView.
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

## Plano de implementação

Segue o processo obrigatório de `CONTRIBUTING.md` § "Processo para mudar o
wire format" para mudanças de bytes/interpretação/garantias do wire:

1. **Motivação e impacto nos três consumidores** — coberto nesta ADR;
   `bally_dongle` e `bally_OS` ganham cripto acelerada por hardware, o
   TraceView usa AES-NI via mbedtls/OpenSSL. Nenhum dos três perde
   compatibilidade com tráfego não cifrado.
2. **ADR** — este documento (0012). Passa para `Aceita` quando os itens
   abaixo estiverem implementados e testados.
3. **Atualização da especificação canônica** (`BTP_V1.md`) — pendente: nova
   seção normativa de criptografia AEAD, entrada `ENCRYPTED` na tabela de
   flags, atualização de `version` do wire e do documento para `2.0.0`.
4. **Classificação SemVer** — `MAJOR`, `v2.0.0`: decisores acordaram tratar
   como incompatível porque um decoder v1.x **MUST** rejeitar qualquer frame
   com bit reservado marcado (`BTP_V1.md` §5), logo não é seguramente
   ignorável por um peer antigo mesmo sendo opcional em uso.
5. **Vetores de conformidade** — pendente: casos válidos (round-trip
   cifrado) e inválidos (tag corrompido, AAD alterado, payload truncado
   antes do tag) em um novo diretório `test-vectors/v2/`.
6. **Implementação equivalente nas plataformas afetadas** — pendente:
   `btp::codec`/`btp::fragmentation` (biblioteca compartilhada), wiring de
   `mbedtls_gcm_*` em `bally_dongle`/`bally_OS` (ESP-IDF/Arduino) e em
   TraceView (Qt/C++).
7. **Estratégia de negociação** — pendente: confirmar se `ENCRYPTED` é
   decisão estática de configuração por par (dongle/robô já provisionados
   com a mesma chave, sem negociação em runtime) ou se precisa de sinal
   explícito no handshake `HELLO` existente.
8. **Notas de migração** — a linha v1.x continua servida pela branch `1.x`
   (cortada do último commit da linha 1, `d736c50`, antes desta ADR). v2.0
   não terá modo legado, parser alternativo nem fallback para payload em
   claro dentro do mesmo canal, por princípio já registrado em
   `CONTRIBUTING.md`.

## Impacto de versão

Classificado como `MAJOR` — `v2.0.0` (`docs/VERSIONING.md`). Branch de
manutenção `1.x` cortada do commit `d736c50` antes desta mudança começar a
avançar em `main`, conforme "Branches de release".
