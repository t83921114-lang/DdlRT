# 项目根目录（与 update_all.sh rsync 目标一致）
if [ -z "${PROJECT_ROOT:-}" ]; then
  PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  export PROJECT_ROOT
fi
