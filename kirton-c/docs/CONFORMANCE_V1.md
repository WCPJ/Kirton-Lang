# Kirton 1.0 — обязательная conformance-матрица

Это конечный список обязательных проверок для Kirton 1.0. Нормативные правила
языка находятся в [`SYNTAX_V1.md`](SYNTAX_V1.md); критерии выпуска, статусы,
ограничения и итоговые результаты — в [`RELEASE_GATE_V1.md`](RELEASE_GATE_V1.md).
Статус «проверено» разрешён только после успешного выполнения указанной
проверки на заявленном target. Наличие теста само по себе не является доказательством.

| ID | Обязательное требование | Позитивная проверка | Негативная проверка / границы |
|---|---|---|---|
| LEX-01 | Регистр и алфавит идентификаторов (латиница/кириллица в начале; `_`, цифры и combining marks в продолжении); формы комментариев и закрытие блочного комментария | `kirton_lexer_contract` (`tests/lexer_contract.c`): `name7_part`, `_private`, `имя́` | `kirton_lexer_contract`: unsupported symbol, combining mark at start, unterminated comment |
| LEX-02 | Строки UTF-8, обязательные escape, запрет неизвестных escape, malformed UTF-8 и сырых управляющих символов | `kirton_string_escapes` | `kirton_lexer_contract`, `kirton_rejects_unknown_string_escape` |
| VAL-01 | Виды значений, точные большие целые и равенство вложенных/циклических коллекций | `kirton_value_contract`, `kirton_value_numeric_conversions`, `kirton_bigint_retention`, `kirton_collection_alias_memory` | Ошибочные типовые операции в `kirton_operator_contract`; JSON-числа в `kirton_json_numbers` |
| VAL-02 | Truthiness согласно спецификации | `kirton_truthiness`, `kirton_truthiness_resource` | В каждом сценарии проверяются ложные пустые значения и истинные остальные значения |
| MEM-01 | Корни GC/pin, достижимость значений, VM-владение, closure lifetime, borrow/lease и явное закрытие ресурса | `kirton_gc_runtime`, `kirton_gc_pin_values`, `kirton_value_bigint_roots`, все `kirton_memory_v2_*`, `kirton_memory_v2_resource_lease`, `kirton_native_resource_finally`, `kirton_memory_closure_error` | В набор входят stale/foreign handle, тип, диапазон, выравнивание и ошибка ресурса |
| SCOPE-01 | Объявление `пусть`, затенение, повторное объявление и присваивание ближайшей привязке | `kirton_scope_bindings` | `kirton_rejects_unknown_variable`, `kirton_rejects_assignment_to_unknown_variable` |
| FUNC-01 | Арность, порядок аргументов, возврат, рекурсия, вложенные функции и живые захваченные привязки | `kirton_function_contract`, `kirton_closures` | `kirton_rejects_wrong_arity`, `kirton_rejects_extra_arguments` |
| FLOW-01 | `если`, циклы, `для каждого`, `прервать`, `продолжить`, возврат из цикла и граница функции | `kirton_flow_contract`, `kirton_loop_control`, `kirton_finally_loop_flow` | `kirton_rejects_break_outside_loop`, `kirton_rejects_continue_outside_loop`, `kirton_rejects_break_inside_function` |
| EXPR-01 | Приоритет/ассоциативность, короткое замыкание, типы операторов, точная целая арифметика, floor-деление, остаток, битовые операции и сдвиги | `kirton_operator_contract`, `kirton_numeric_operators`, `kirton_numeric_special` | `kirton_rejects_bitwise_float`, `kirton_rejects_negative_shift`, `kirton_rejects_non_integer_shift`, `kirton_rejects_division_by_zero_in_integer_division`, `kirton_rejects_modulo_by_zero` |
| STR-01 | Неизменяемая индексация Unicode-строки кодпоинтами и ошибки недопустимого индекса | `kirton_string_index`, `kirton_string_index_api` | `kirton_rejects_string_index_out_of_range`, `kirton_rejects_negative_string_index`; malformed UTF-8 в C API проверяет `kirton_string_index_api` |
| ERR-01 | Ближайший handler, форма ошибки/координаты/стек, import/native errors, раскрутка и приоритет cleanup | `kirton_error_handling`, `kirton_native_error_catch`, `kirton_keeps_imported_source_location`, `kirton_finally_error_overrides` | `kirton_unhandled_error_runs_finally`, `kirton_reports_division_error_location`, `kirton_stops_on_import_runtime_error` |
| TYPE-01 | Структуры/enum: конструирование, идентичность экземпляра, различие одноимённых вариантов enum разных типов, проверка полей при создании/присваивании, GC и межмодульная передача | `kirton_user_types` (`tests/user_types.kir` проверяет identity структур и enum), `kirton_user_type_integration` | `kirton_rejects_invalid_user_type_field`, `kirton_rejects_missing_user_type_field`, `kirton_rejects_invalid_user_type_assignment`, `kirton_rejects_unknown_enum_variant`, `kirton_rejects_duplicate_user_type_member` |
| MOD-01 | Порядок поиска, пути/суффиксы, кэширование один раз, namespace exports, private names, alias scoping и изоляция VM | `kirton_module_path_contract`, `kirton_module_exports`, `kirton_module_function_scope`, `kirton_vm_import_isolation`, `kirton_module_cache_growth`, `kirton_scoped_import_aliases` | `kirton_rejects_unqualified_imported_function`, `kirton_catches_import_error`, `kirton_catches_cyclic_import`, `kirton_stops_on_import_runtime_error` |
| FFI-01 | Явное opt-in загрузки внешней библиотеки, ABI V2/V3, schema/resource API и перевод callback error в runtime error | `kirton_ffi_v2_api`, `kirton_ffi_v3_api`, `kirton_ffi_v2_cli`, `kirton_ffi_v3_cli`, `kirton_native_error_catch` | `kirton_ffi_v2_cli_requires_opt_in`, `kirton_ffi_disabled_by_default`; bad ABI cases входят в API tests |
| NATIVE-01 | Проверки входных размеров, HTTP GET/POST loopback behavior и контролируемые ошибки allocation в перечисленных core modules | `kirton_http_loopback`, `kirton_native_input_fuzz`, `kirton_json_depth`, `kirton_json_numbers`, `kirton_bitmap_dimensions`, `kirton_*_oom` | OOM-модули, перечисленные в разделе ограничений gate, не считаются полным аудитом всех native modules; внешние HTTP endpoints не входят в suite |
| CLI-01 | Help/usage, entrypoint, FFI opt-in, аргументы после `--`, коды завершения и UTF-8 путь | `kirton_cli_contract`, `kirton_cli_utf8_script_path` | `kirton_cli_contract` проверяет неверные опции/пути/аргументы и коды возврата |
| APP-01 | Установочное руководство и запуск многофайловых приложений | `kirton_core_smoke`, `kirton_task_tracker_example`, `kirton_expense_report_example`; установленный package smoke описан в release gate | Пакетный smoke запускается из install tree, а не из source tree |
| PLAT-01 | Сборка, обязательные проверки и установленный пакет на каждой заявленной платформе | Release Gate: точные toolchain/configuration/команды и результат по каждому target | Не прошедшая или недоступная платформа не включается в список поддержки |

## Область и исключения

Эта матрица проверяет спецификацию языка, CLI и контракт базовых API, описанных
в спецификации. Coverage-guided fuzzing, ручное GUI/audio и реальные GPU/async
драйверы не являются заменой перечисленным проверкам и не заявляются как
прошедшие. Их статус и релизная классификация указаны в release gate. POSIX
варианты процессов/HTTP и process-global legacy graphics/audio не входят в
заявленную область Windows x64/MSVC 1.0.
