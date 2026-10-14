# compbench

순수 재귀 AST를 x86-64/AVX2 어셈블리로 최적화하는 C 코드 생성기 벤치마크다. 언어·AST·파서·참조 실행기·베이스라인·하네스·문제군·모델 프롬프트를 **v1과 v2로 분리**한다.

| | v1 — 배열 값 추가 전 | v2 — 배열 값 추가 후 |
| --- | --- | --- |
| 값 타입 | `u64` | `u64`, 불변 `list<u64>` |
| 입력 | 읽기 전용 배열의 `len/get` | `len/get` 및 리스트 값 `input` |
| 배열 값 연산 | 없음 | `nil/cons/head/tail/is_nil` |
| 함수 인자·반환값 | 정수 | 정수 또는 리스트 |
| 생성 코드 진입 ABI | `bench_entry(xs, n)` | `bench_entry(xs, n, arena)` |
| 기본 문제 | 스칼라 26개 | 스칼라 8개 + 리스트 6개 |
| 고정 코드·명세 | [`v1/`](v1/README.md) | [`v2/`](v2/README.md) |
| 모델 프롬프트 | [`v1/prompts/compiler.md`](v1/prompts/compiler.md) | [`v2/prompts/compiler.md`](v2/prompts/compiler.md) |
| 제출 소스 | `res/v1/*.c` | `res/v2/*.c` |
| 평가 JSON | `build/res/v1/*.json` | `build/res/v2/*.json` |

v1에도 SIMD 최적화를 위한 입력 배열은 있다. v2의 추가 기능은 배열을 재귀 리스트 형태의 값으로 만들어 함수에 전달·반환·공유하는 것이다. 두 버전 모두 unsigned 64비트 wraparound 산술이며 나누기와 mod의 분모가 0이면 결과는 0이다. 파서는 모델이 수정하지 않는다.

## 빌드와 검증

저장소 루트에서 실행한다. Linux x86-64, AVX2를 지원하는 CPU/OS, C11 컴파일러와 GNU 호환 어셈블러·링커가 필요하다.

```sh
make                  # 두 버전 빌드
make v1               # v1만 빌드
make v2               # v2만 빌드
make demo-v1          # v1 베이스라인: 26개 문제
make demo-v2          # v2 베이스라인: 14개 문제
make test             # 버전별 의미·통합 검사와 버전 경계 검사
```

실행 파일은 `build/v1/`, `build/v2/`에 생성한다. `make demo`는 v2를 실행하며, 자기 베이스라인과 비교하므로 배율은 약 1이다.

## 모델 프롬프트와 제출물 평가

선택한 버전의 `prompts/compiler.md`를 모델에게 전달한다. C stub, 해당 버전의 AST 구조와 의미, 베이스라인 codegen 전체가 들어 있다. 모델은 설명이나 `main()` 없이 `cb_emit_assembly()`를 구현한 C 소스만 출력한다.

```sh
make prompt                              # 두 프롬프트 갱신
make prompt-v1                           # v1만 갱신
make prompt-v2                           # v2만 갱신
python3 scripts/build_prompt.py --check   # 두 프롬프트와 소스 일치 검사

make evaluate-v1                         # res/v1/*.c 평가
make evaluate-v2                         # res/v2/*.c 평가
python3 scripts/evaluate.py --version v1 gemini-4-argon
python3 scripts/evaluate.py --version v2 model-name
```

`res/v2/`는 새 배열 프롬프트의 제출물을 담는 곳이다. 제출물이 없으면 평가 스크립트가 이를 알리고 종료한다. 기존 `scripts/evaluate_v1.py`도 계속 사용할 수 있으며, v2용 `scripts/evaluate_v2.py`를 제공한다.

직접 빌드할 때도 같은 버전의 헤더·파서·진입 코드만 링크한다:

```sh
cc -std=c11 -O2 -Iv1/include v1/src/baseline.c v1/src/ast.c solution.c -o build/v1/solution
./build/v1/compbench --compiler ./build/v1/solution --output build/v1/results.json

cc -std=c11 -O2 -Iv2/include v2/src/baseline.c v2/src/ast.c solution.c -o build/v2/solution
./build/v2/compbench --compiler ./build/v2/solution --output build/v2/results.json
```

각 하네스의 기본 suite는 각각 `v1/benchmarks/suite.tsv`(26개), `v2/benchmarks/suite.tsv`(14개)다. `--suite PATH`, `--seed U64`, `--cc PATH`, `--keep-artifacts`를 사용할 수 있다. manifest의 열은 난이도·측정 입력 길이·AST 경로이며 AST 경로는 manifest 기준이다. v1의 예전 8개 문제는 `v1/benchmarks/suite-original.tsv`로 선택할 수 있다.

## 결과와 그래프

결과 JSON에는 `benchmark_version`, `entry_abi`, `suite`, `baseline`, `harness`를 기록한다. JSON 형식의 `schema_version: 2`와 언어 버전 v1/v2는 별개다. 제출물 평가 스크립트는 소스 경로·SHA-256·suite SHA-256·컴파일 로그도 기록한다.

전체 점수는 **성공한 문제의 `baseline_ns / candidate_ns`와 실패한 문제의 1.0을 모두 포함한 기하평균**이다. 분모는 전체 문제 수이며, 성공한 문제의 배율이 1 미만이면 그대로 반영한다. 모든 문제가 실패하거나 제출물 컴파일이 실패하면 점수는 1.0이다. 오답·크래시·시간 초과는 실패 상태와 통과 개수에 기록한다. JSON은 `score_policy: "geomean_failures_as_1"`, `failure_speedup: 1.0`도 포함한다.

기존 측정값으로 점수만 다시 계산하려면 `python3 scripts/rescore_results.py`를 실행한다. 활성 결과와 분석 JSON을 갱신하고 변경 전 파일을 `build/archive/pre-failure-scoring/`에 보관한다. 그래프도 저장된 문제별 배율로 같은 규칙을 적용한다.

```sh
python3 -m pip install -r requirements-viz.txt
make charts-v1
make charts-v2
python3 scripts/render_results.py --version v2 --formats png svg pdf
```

모델 그래프의 입력과 출력은 각각 `build/res/<version>/`, `build/figures/<version>/`다. `make charts`는 v1을 렌더링한다. 서로 다른 버전·ABI·문제군·베이스라인·하네스의 JSON은 한 비교 그래프에 넣지 않는다. `--input-dir`, `--output-dir`, `--no-analysis`, `--analysis PATH`로 조정할 수 있다.

문제별 히트맵은 한 묶음에 최대 9개씩 여러 줄로 배치한다. 26개 문제는 9·9·8개로 나누고 모델 순서와 색상 배율은 모든 묶음에 동일하게 적용한다. `--columns N`으로 한 줄의 최대 문제 수를 조절할 수 있다.

버전 분리 전 평가 결과·컴파일 로그·바이너리·분석 산출물·그래프와 기존 파일의 사본은 `build/archive/pre-version-split/`에 보존했다. 이전 v1 평가는 리스트 지원 하네스에 스칼라 베이스라인을 링크해 측정 루프에 arena 초기화가 포함되어 있었다. 분리된 v1은 정확한 2인자 ABI를 사용하며 arena 초기화를 수행하지 않는다. 과거 결과의 수치나 provenance는 고쳐 쓰지 않았고, 새 평가 결과와 섞어서 비교하지 않는다. 이 로컬 archive는 `make clean` 때 함께 삭제된다.

## 생성 코드 분석

다음 스크립트들은 모두 분리된 v1의 고정 헤더·파서·문제군·하네스를 사용한다. 원본 제출 소스를 수정하지 않고 `build/analysis/`에 분석용 복사본과 어셈블리를 저장한다.

```sh
make v1
python3 scripts/analyze_v1.py
python3 scripts/audit_v1_constant_folding.py
python3 scripts/compare_v1_max.py
python3 scripts/analyze_v1_argon.py
```

분리 전 측정값의 분석 기록은 [벡터화·재귀 변환 분석](docs/v1-codegen-analysis.md), [Astra max와 exp-a 비교](docs/astra-max-vs-exp-a.md), [Gemini 4 Argon 분석](docs/gemini-4-argon-analysis.md)에 있다. 재실행한 결과는 새 v1 하네스의 결과이며 문서에 남은 과거 측정값과 차이가 날 수 있다.

[Argon high 최적화 분석](docs/argon-high-optimization-coverage.md)을 바탕으로 만든 최적화 문제 18개를 v1 정규 suite에 합쳤다. `make evaluate-v1`과 `make demo-v1`은 총 26개를 평가한다. `v1/benchmarks/suite-original.tsv`는 예전 8개, `suite-optimizations.tsv`는 최적화 18개를 선택하며, `suite-extended.tsv`는 정규 suite와 같은 26개를 담은 호환 manifest다. 이전 8개 평가 결과·그래프는 `build/archive/pre-regular-optimizations/`에 보관했다.

이전 분석 스크립트 `analyze_v1.py`, `compare_v1_max.py`, `analyze_v1_argon.py`, `audit_v1_constant_folding.py`는 과거 비교를 위해 `suite-original.tsv`를 사용한다. `make test-optimizations`로 독립적인 정답 검사를 실행하고 `python3 scripts/analyze_argon_high.py`로 정규 26개에 대한 최적화 제거 실험을 재현한다.
