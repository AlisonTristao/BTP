"""Hooks de build do MkDocs para o livro do BTP.

Os documentos de `docs/` são lidos em dois lugares: no GitHub, onde um link
relativo como `../src/codec.cpp` resolve porque o repositório inteiro está lá,
e no site publicado, onde não resolve porque só `docs/` é copiado para dentro
do site. Em vez de trocar esses links por URLs absolutas no texto -- o que
quebraria a leitura direto do repositório e o `git grep` --, este hook os
reescreve em tempo de build, e somente os que de fato escapam da árvore de
documentação.
"""

import posixpath
import re

# Branch usada para montar as URLs de código-fonte. Precisa acompanhar a branch
# que o workflow de publicação (.github/workflows/docs.yml) constrói.
SOURCE_BRANCH = "main"

# Captura o destino de qualquer link markdown que comece por "./" ou "../".
# Links absolutos (http, mailto) e âncoras internas ficam de fora por
# construção, e é isso que se quer: eles já funcionam nos dois lugares.
RELATIVE_LINK = re.compile(r"\]\((\.{1,2}/[^)\s]+)\)")


def on_page_markdown(markdown, page, config, files, **kwargs):
    repo_url = (config.get("repo_url") or "").rstrip("/")
    if not repo_url:
        return markdown

    page_dir = posixpath.dirname(page.file.src_uri)

    def rewrite(match):
        target = match.group(1)
        # normpath descarta a barra final, mas ela é o único sinal disponível
        # para diferenciar diretório de arquivo -- e o GitHub usa caminhos
        # diferentes para cada caso (`tree` e `blob`).
        is_directory = target.endswith("/")
        repo_relative = posixpath.normpath(posixpath.join("docs", page_dir, target))

        # Ainda dentro de docs/: o MkDocs resolve sozinho, não se toca.
        if repo_relative == "docs" or repo_relative.startswith("docs/"):
            return match.group(0)

        kind = "tree" if is_directory else "blob"
        return "]({}/{}/{}/{})".format(repo_url, kind, SOURCE_BRANCH, repo_relative)

    return RELATIVE_LINK.sub(rewrite, markdown)
