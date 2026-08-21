# BTP v2 conformance vectors (encaixe de AEAD)

Sibling de [`test-vectors/v1/`](../v1/README.md), mesmo contrato de arquivo
(par `.json`/`.bin`, gerados e verificados por
[`tools/test_vectors_v2.py`](../../tools/test_vectors_v2.py)), cobrindo a
extensao normativa da [secao 8 de `BTP_V1.md`](../../docs/BTP_V1.md#8-criptografia-aead-do-payload)
(flag `ENCRYPTED`, octeto de versao 2, sub-campo `CIPHER_ID`,
[ADR 0012](../../docs/decisions/0012-criptografia-aead-payload.md)).
`tools/test_vectors_v2.py` e' um script independente de
`tools/test_vectors.py` -- nao importa nem depende dele -- exatamente como a
v2 tem sua propria reimplementacao de referencia do decode, separada da v1.

Verificacao rapida, a partir da raiz do repositorio:

```text
python tools/test_vectors_v2.py --root test-vectors/v2 --check
```

## O que esta suite prova

- `valid/hello.json`: um frame com `ENCRYPTED` limpo e' byte a byte identico
  ao `test-vectors/v1/valid/hello.bin` -- o encaixe de AEAD nao alterou nada
  no caminho nao cifrado (secoes 1-7 de `BTP_V1.md`).
- `valid/aead_telemetry_gcm.json`: um frame com `ENCRYPTED` marcado cujo
  `payload_hex` e' ciphertext‖tag **real** de AES-128-GCM (nao um
  placeholder), com chave/nonce/AAD/plaintext documentados no bloco `"aead"`
  do JSON para reproducao e verificacao cross-plataforma futura. `flags`
  tambem exercita o valor normativo do sub-campo `CIPHER_ID` (bits 2-3,
  BTP_V1.md secao 5/8.1) para AES-128-GCM: `0`, documentado explicitamente
  como `"cipher_id": 0` no bloco `"aead"`.
- `invalid/encrypted_version_mismatch.json`: `ENCRYPTED` marcado com octeto
  de versao 1 -- `btp::Error::EncryptedVersionMismatch`.
- `invalid/crc_mismatch_encrypted.json`: CRC do envelope corrompido sobre um
  frame `ENCRYPTED` -- prova que o framing (CRC, seccao 7) continua
  funcionando identicamente independente da flag.
- `invalid/reserved_flag.json`: bit `0x0010` (ainda reservado; `0x0002` e
  `0x000C` deixaram de ser reservados ao virar `ENCRYPTED` e `CIPHER_ID`) --
  `btp::Error::InvalidFlags`.
- `invalid/cipher_id_reserved.json`: `ENCRYPTED` marcado com `CIPHER_ID == 2`
  (valor reservado para cifras futuras, BTP_V1.md secao 8.1) --
  `btp::Error::InvalidCipherId`.
- `invalid/cipher_id_requires_encrypted.json`: `ENCRYPTED` limpo com
  `CIPHER_ID == 1` -- nao ha cifra "em uso" por um frame nao cifrado, entao
  `CIPHER_ID` diferente de zero e' rejeitado mesmo sendo um valor atribuido
  -- `btp::Error::InvalidCipherId`.

## O que esta suite deliberadamente NAO cobre

`btp::codec` (`include/btp/codec.hpp`/`src/codec.cpp`) so entende framing:
magic, versao, tamanho, CRC do envelope, flags, fragmentacao. Quando
`ENCRYPTED` esta marcado, o payload de um frame e' tratado como bytes opacos
(`ciphertext ‖ tag`) do ponto de vista de `decode()` -- a biblioteca nao
chama AES-128-GCM/ChaCha20-Poly1305 nem compara o tag de autenticacao.

Por isso, vetores "invalidos" do tipo *tag corrompido, mas estruturalmente
valido* (por exemplo: ciphertext alterado, AAD/header alterado depois de
cifrar, ou tag truncado/zerado) **nao existem nesta suite**. Um vetor assim
seria uma alegacao falsa contra `btp::decode()`: a funcao aceitaria o frame
normalmente (version==2, `ENCRYPTED` presente, `payload_size` batendo,
CRC do envelope intacto) porque nao tem como saber que o tag nao autentica o
payload.

Essa verificacao criptografica de verdade fica **reservada para uma etapa
futura de teste no lado de cada consumidor** com uma biblioteca AEAD real
linkada (`mbedtls_gcm_*`/ChaCha20-Poly1305 em `bally_dongle`/`bally_OS` via
ESP-IDF/Arduino, ou Qt/OpenSSL/mbedtls em TraceView -- ver o item 6 do plano
de implementacao em `docs/decisions/0012-criptografia-aead-payload.md`).
Quando esse wiring existir, o vetor `valid/aead_telemetry_gcm.json` desta
suite serve como caso positivo de interoperabilidade (chave/nonce/AAD/
plaintext documentados no bloco `"aead"`), e a suite de cada consumidor deve
acrescentar os proprios casos negativos de tag invalido contra a sua propria
chamada de verificacao -- essa suite aqui nao e' o lugar certo para eles.
