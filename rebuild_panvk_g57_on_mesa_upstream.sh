#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail

SRC="$HOME/mesapvk/mesasrc"
WORK="$HOME/mesapvk/mesa-panvk-g57-git"
UPSTREAM="https://gitlab.freedesktop.org/mesa/mesa.git"
GITHUB="https://github.com/mexicanbr0auth/mesa-panvk-g57.git"
REF_REPO="https://github.com/wonderkast02/panvk-g720-kbase-csf.git"
REPORT="/sdcard/mesa_panvk_g57_git_import.txt"

exec > >(tee "$REPORT") 2>&1

echo "============================================================"
echo " Mesa PanVK G57 - reconstruir sobre Git real do Mesa"
echo "============================================================"
date

[ -d "$SRC" ] || { echo "ERRO: $SRC nao existe"; exit 1; }

# Never touch/delete the working source tree.
if [ -e "$WORK" ]; then
  echo "ERRO: destino ja existe: $WORK"
  echo "Nada foi apagado. Renomeie/remova esse destino somente se voce souber o que ha nele."
  exit 2
fi

echo
echo "===== 1. CLONANDO MESA UPSTREAM COM HISTORICO ====="
git clone "$UPSTREAM" "$WORK"
cd "$WORK"

git remote rename origin upstream
git remote add origin "$GITHUB"
git remote add g720-reference "$REF_REPO"

echo
echo "===== 2. TENTANDO IDENTIFICAR A BASE DA ARVORE ATUAL ====="
echo "VERSION local:"
cat "$SRC/VERSION" 2>/dev/null || true

# Find a plausible upstream commit by matching blob hashes for files that are
# very unlikely to have been changed by the G57 work. We score recent commits
# and choose the best exact-content match, but do NOT destroy the original tree.
python3 - "$SRC" "$WORK" > /tmp/panvk_base_commit.txt <<'PY'
import hashlib, os, subprocess, sys

src, repo = sys.argv[1:3]
files = [
    "README.rst",
    "VERSION",
    "clippy.toml",
    "rustfmt.toml",
    "CODEOWNERS",
    "licenses/MIT",
    "src/util/bitset.h",
    "src/util/macros.h",
]

def git(*args, input=None):
    return subprocess.check_output(["git", "-C", repo, *args], input=input).decode().strip()

local = {}
for f in files:
    p = os.path.join(src, f)
    if os.path.isfile(p):
        data = open(p, "rb").read()
        hdr = f"blob {len(data)}\0".encode()
        local[f] = hashlib.sha1(hdr + data).hexdigest()

# Search recent upstream history. 6000 commits is intentionally broad enough
# for a development snapshot without traversing Mesa's entire history.
commits = git("rev-list", "--first-parent", "--max-count=6000", "upstream/main").splitlines()

best = None
for idx, c in enumerate(commits):
    score = 0
    possible = 0
    for f, want in local.items():
        try:
            got = git("rev-parse", f"{c}:{f}")
        except subprocess.CalledProcessError:
            continue
        possible += 1
        if got == want:
            score += 1
    key = (score, -idx, possible, c)
    if best is None or key > best:
        best = key
    # All sampled files match: newest such commit is good enough.
    if possible and score == possible and possible >= 5:
        best = key
        break

if not best:
    raise SystemExit("BASE_NOT_FOUND")

score, negidx, possible, commit = best
print(commit)
print(f"SCORE={score}/{possible}", file=sys.stderr)
PY

BASE="$(head -n1 /tmp/panvk_base_commit.txt)"
echo "Base candidata: $BASE"
git show -s --format='commit=%H%ndate=%cI%nsubject=%s' "$BASE"

echo
echo "===== 3. CRIANDO BRANCH DO PROJETO ====="
git switch -c mali-g57-kbase-jm "$BASE"

echo
echo "===== 4. SOBREPOR A ARVORE DE DESENVOLVIMENTO ====="
# Copy the current source snapshot over the matching Mesa base.
# Exclude every local build tree, git metadata, backup/log/binary artifacts,
# and the bootstrap scripts themselves.
tar -C "$SRC" \
  --exclude='./.git' \
  --exclude='./build-bionic' \
  --exclude='./build-gcc' \
  --exclude='./build-linux' \
  --exclude='./build-min' \
  --exclude='./build-panvk' \
  --exclude='./build-support' \
  --exclude='./build' \
  --exclude='./*.bak' \
  --exclude='./*.bak-*' \
  --exclude='./*.orig' \
  --exclude='./*.rej' \
  --exclude='./setup_mesa_panvk_g57_github.sh' \
  --exclude='./fix_init_and_push_mesa_panvk_g57.sh' \
  -cf - . | tar -C "$WORK" -xf -

cat >> .gitignore <<'EOF'

# mesa-panvk-g57 local development
build-bionic/
build-gcc/
build-linux/
build-min/
build-panvk/
build-support/
build*/
*.bak
*.bak-*
*.va32-probe-*
*.orig
*.rej
panvk_*.txt
*_result.txt
*_diag.txt
frame.png
*.swp
*.swo
*~
EOF

echo
echo "===== 5. AUDITORIA DO DIFF ====="
CHANGED="$(git status --porcelain | wc -l | tr -d ' ')"
echo "Entradas modificadas/untracked: $CHANGED"
git status --short
echo
git diff --stat
echo

# Safety: if our guessed base is badly wrong, do not publish a giant accidental
# upstream delta. The user can send REPORT back for adjustment.
TRACKED_CHANGED="$(git diff --name-only | wc -l | tr -d ' ')"
echo "Arquivos rastreados alterados contra a base: $TRACKED_CHANGED"

if [ "$TRACKED_CHANGED" -gt 250 ]; then
  echo
  echo "ABORTADO COM SEGURANCA:"
  echo "A base candidata gerou >250 arquivos upstream alterados."
  echo "Nada foi commitado ou enviado ao GitHub."
  echo "Envie este arquivo para o ChatGPT:"
  echo "$REPORT"
  exit 3
fi

echo
echo "===== 6. PARANDO PARA AUDITORIA ====="
echo
echo "IMPORTACAO PREPARADA, MAS NADA FOI COMMITADO OU ENVIADO."
echo "Repo de auditoria: $WORK"
echo
echo "Envie o relatório:"
echo "$REPORT"
echo
echo "Depois da revisão faremos commit/push separadamente."
exit 0
