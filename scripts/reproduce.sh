#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/.." && pwd)"
cd "$project_dir"

sizes="${SIZES:-8,16,32,64,128}"
threads="${THREADS:-10}"
warmups="${WARMUPS:-2}"
repeats="${REPEATS:-10}"
output_dir="${OUTPUT_DIR:-results}"
python_bin="${PYTHON:-python3}"

case "${1:-paper}" in
  paper) ;;
  smoke)
    sizes="${SIZES:-8,16}"
    threads="${THREADS:-2}"
    warmups="${WARMUPS:-1}"
    repeats="${REPEATS:-2}"
    ;;
  *) echo "Usage: scripts/reproduce.sh [paper|smoke]" >&2; exit 2 ;;
esac

make native
make test
mkdir -p "$output_dir"

for protocol in fdvss1 fdvss2; do
  ./fdvss/build/fdvss_bench \
    --protocol "$protocol" --n "$sizes" --threads "$threads" \
    --warmups "$warmups" --repeats "$repeats" \
    --csv "$output_dir/$protocol.csv" --json "$output_dir/$protocol.json"
done

./fdpss/build/fdpss_bench \
  --protocol fdpss2 --n "$sizes" --threads "$threads" \
  --warmups "$warmups" --repeats "$repeats" \
  --csv "$output_dir/fdpss2.csv" --json "$output_dir/fdpss2.json"

"$python_bin" -B scripts/summarize.py \
  "$output_dir/fdvss1.csv" "$output_dir/fdvss2.csv" "$output_dir/fdpss2.csv" \
  --output-dir "$output_dir/summary"
