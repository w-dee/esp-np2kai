# Project instructions

- Never place developer-specific absolute home paths in tracked files.
- Use repository-relative paths, `$HOME`, `${HOME}`, `~/`, or documented placeholders.
- Run the repository privacy lint before committing.
- Enable the local hook with `git config core.hooksPath .githooks`.

- サンドボックス内からは /dev/ 以下のシリアルデバイスが見えないかもしれません。必要ならば昇格してください。
- esptool.py で SoC からの応答がない場合は、3 回まで試行してください。それでも応答がなければ失敗としてください。