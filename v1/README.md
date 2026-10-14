# compbench v1 — 배열 값 추가 전

v1은 모든 표현식·함수 인자·반환값이 `u64`인 언어다. 각 함수는 같은 읽기 전용 입력 배열을 `len`과 `(get i)`로 읽는다. 배열을 값으로 생성·전달·반환하는 연산은 없다. v2의 `input/nil/cons/head/tail/is_nil` 연산은 이 파서에서 거절한다. `input`, `nil`이라는 이름을 일반 지역 변수로 바인딩하는 것은 허용한다.

모든 명령은 저장소 루트에서 실행한다. 전체 버전 구성은 [루트 README](../README.md)에 있다.

```sh
make v1
make demo-v1
make test-v1
make evaluate-v1
```

- [고정 AST 헤더](include/ast.h)
- [고정 파서와 참조 실행기](src/ast.c)
- [스칼라 베이스라인](src/codegen.c)
- [하네스](src/harness.c)
- [정규 문제군 26개](benchmarks/suite.tsv)
- [이전 문제군 8개](benchmarks/suite-original.tsv)
- [모델 프롬프트](prompts/compiler.md)

## 의미와 ABI

```c
uint64_t bench_entry(const uint64_t *xs, uint64_t n);
```

System V AMD64 호출 규약을 지킨다. 입력 배열은 읽기 전용이며 범위 밖 원소 읽기 결과는 0이다. 실제로 범위 밖 주소를 읽거나 길이 0인 입력을 역참조하면 안 된다. 함수 인자·반환값과 지역 바인딩에는 정수만 사용한다.

| AST | 의미 |
| --- | --- |
| 정수 리터럴, 변수 | unsigned 64비트 값 |
| `len`, `(get i)` | 입력 배열의 길이, 원소; 범위 밖은 0 |
| `(add a b)`, `(sub a b)`, `(mul a b)` | modulo 2^64 산술 |
| `(div a b)`, `(mod a b)` | unsigned 몫·나머지; 분모 0이면 0 |
| `(and a b)`, `(or a b)`, `(xor a b)` | 비트 연산 |
| `(shl a b)`, `(shr a b)` | 이동량 `b & 63`; 오른쪽은 논리 시프트 |
| `(eq a b)`, `(ne a b)`, `(lt a b)`, `(le a b)`, `(gt a b)`, `(ge a b)` | unsigned 비교, 결과 0 또는 1 |
| `(if condition yes no)` | 선택한 분기만 평가; 0이면 거짓 |
| `(let name value body)` | 어휘적 바인딩; 값은 바깥 범위에서 평가 |
| `(call function arg ...)` | 순수 함수 호출; 직접·상호·비꼬리 재귀 |

함수 64개, 인자 4개, 함수당 지역 슬롯 128개, AST 노드 4096개, AST 중첩 256단계, 파일 1 MiB 한도를 사용한다. 참조 평가 예산은 2천만 노드, 비꼬리 평가 깊이는 256단계이며 꼬리 호출은 반복 처리한다. `main`은 인자가 없어야 한다.

정규 문제군은 원래의 `sum`, `unsigned_max`, `conditional_sum`, `divmod`, `mutual_recursion`, `helper_mix`, `non_tail_sum`, `fibonacci`와 최적화 문제 18개를 합친 26개다. 보존된 스칼라 베이스라인을 사용한다. 배열 입력 보호, 실패·시간 초과 처리, 같은 CPU에서의 교대 측정과 7회 중앙값을 유지한다. 입력 리스트·arena를 구성하거나 초기화하지 않는다.

제출물은 `res/v1/*.c`, 새 평가 JSON은 `build/res/v1/*.json`에 둔다. v1의 AST 구조체에는 리스트 타입 필드가 없다. 기존 모델 소스도 이 헤더와 고정 파서로 다시 빌드한다.

```sh
cc -std=c11 -O2 -Iv1/include v1/src/baseline.c v1/src/ast.c solution.c -o build/v1/solution
./build/v1/compbench --compiler ./build/v1/solution --output build/v1/results.json
```

JSON에는 `benchmark_version: "v1"`과 `entry_abi: "u64(xs,n)"`를 기록한다. 현재의 2인자 하네스는 버전 분리 때 정수 전용으로 구성한 것이며, 과거 하네스 전체의 바이트 단위 복원이라는 뜻은 아니다. 분리 전 측정값과 새 측정값은 별도로 보존·비교한다.

전체 점수는 성공한 문제의 실측 배율과 실패한 문제의 1.0을 전체 문제 수로 기하평균한다. 실패한 문제를 분모에서 제외하지 않으며, 전부 실패하면 1.0이다. 실패 상태·통과 개수·오류 메시지도 함께 기록한다.

## 정규 문제군과 부분집합

기본 `benchmarks/suite.tsv`는 누산·점화식·나눗셈·인덱스 접근·꼬리 호출 인자 보존·재귀 변환을 검사하는 최적화 문제 18개를 포함한 26개 문제다. `make evaluate-v1`, `make demo-v1`과 suite를 지정하지 않은 하네스 실행에 모두 적용된다. `suite-original.tsv`로 이전 8개를, `suite-optimizations.tsv`로 최적화 18개만 선택할 수 있다. 기존 명령의 `suite-extended.tsv`도 같은 정규 26개를 가리킨다. 문제 수와 구성이 다른 점수는 별도로 비교한다.

```sh
make test-optimizations
make evaluate-v1
# 이전 8개만 평가
./build/v1/compbench --compiler ./build/res/v1/gemini-4-argon-high \
  --suite v1/benchmarks/suite-original.tsv --output build/v1/original.json
python3 scripts/analyze_argon_high.py
```

[Argon high 분석과 문제 설계](../docs/argon-high-optimization-coverage.md)에 최적화 제거 실험과 추가 문제에서 확인한 오류를 기록했다. 추가 문제는 `make test-v1`에서도 독립적인 Python 정답과 대조한다.
