# BTP v1 canonical vectors

Cada JSON em `valid/` ou `invalid/` descreve o `.bin` homônimo. Os binários são
frames BTP crus e incluem header, payload e CRC; não incluem COBS nem os
delimitadores `0x00` da serial.

O catálogo e o cenário intercalado estão em [`manifest.json`](manifest.json).
Formato, comandos de verificação, integração com ESP-IDF/Arduino/Qt e política
de alteração estão em [`docs/CONFORMANCE.md`](../../docs/CONFORMANCE.md).

Verificação rápida, a partir da raiz do repositório:

```text
python tools/test_vectors.py --root test-vectors/v1 --check
```
