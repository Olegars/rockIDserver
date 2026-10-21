# rockIDserver

Каркас сервера мессенджера на C++17 для Linux. Модель данных близка к Telegram: пользователь, сессия, личный или групповой диалог, сообщения, контакты, курсор прочитанного и присутствие. Проводной формат намеренно проще MTProto: один TCP-сокет несёт RPC-запросы и серверные пуши, тело кадра — JSON.

Это рабочий каркас, а не законченный клиентский протокол. Сеть, разбор кадров, API и PostgreSQL разделены так, чтобы вложения, каналы и бинарный кодек добавлялись по своим границам.

## Что уже умеет сервер

- Регистрация, вход, облачный пароль 2FA (`account.setCloudPassword`, `auth.checkCloudPassword`) и список сессий с удалённым завершением.
- Личные чаты, секретные чаты с печатью X25519 и AES-GCM, группы до 200 000 участников с темами и ролями, каналы с просмотрами и привязанным чатом обсуждения.
- Текст, голос, видеосообщения, файлы до 2 ГБ (4 ГБ с Premium), ответы, цитаты, правки, удаление, реакции, тихие и отложенные сообщения, подтверждение доставки.
- Эфир: ретрансляция кусков звука, видео и экрана, запись этих кусков в Избранное.
- Избранное, папки, архив, вечный мьют и закрепление чатов.
- Истории на 6, 12, 24 или 48 часов, стикеры, премиум-лимиты и лимит аккаунтов на `device_id`.
- Боты по TCP и по HTTP, HTML мини-приложений, счета со ссылкой `pay_url`.
- Очередь пушей для offline, опциональный zlib и TLS, IPv4 и IPv6. Присутствие пишется в базу и сбрасывается при старте.

## Требования

- Linux с `epoll`. Слушается `AF_INET6` с `IPV6_V6ONLY=0`, поэтому на том же сокете принимается и IPv4. Если IPv6 нет, сервер слушает `AF_INET`.
- Компилятор C++17, проверено на g++ 11.
- CMake 3.16 или новее.
- OpenSSL (`libcrypto` и `libssl`): пароли, TLS, X25519 и AES-GCM.
- zlib.
- PostgreSQL и клиентская библиотека `libpq` (`libpq.so` или `libpq.so.5`). Сервер PostgreSQL должен быть запущен, база и роль создаются отдельно.
- Для перекодирования голоса и видео нужен `ffmpeg`, для `files.edit` у картинок — ImageMagick `convert`. Если программы нет, исходный файл остаётся как есть, а правка отвечает `EDIT_FAILED`.

Заголовок `libpq-fe.h` из пакета `libpq-dev` используется, если он установлен. Если его нет, CMake берёт минимальные объявления из `third_party/libpq-fe.h` и линкует системную библиотеку. Для клиента проверки нужен Python 3 из стандартной библиотеки, без pip-пакетов.

## Сборка и запуск

Из каталога проекта:

```bash
cmake -S . -B build
cmake --build build -j
./build/rockidserver --port 7410 --db "host=/var/run/postgresql dbname=rockid"
```

Роль PostgreSQL должна совпадать с пользователем ОС, от которого запущен процесс: локальный сокет проверяет peer-аутентификацию. Для пользователя `oleg` и базы `rockid`:

```bash
sudo -u postgres psql -d postgres -f deploy/init-postgres.sql
```

Аргументы:

| Аргумент | По умолчанию | Смысл |
| --- | --- | --- |
| `--port N` | `7410` | TCP-порт мессенджера, диапазон 1–65535 |
| `--http N` | порт + 1 | HTTP мини-приложений и ботов; `0` выключает |
| `--files DIR` | `/var/www/rockIDserver/data/files` | каталог загруженных файлов |
| `--tls-cert PEM` | — | сертификат TLS на порту мессенджера |
| `--tls-key PEM` | — | ключ TLS, вместе с сертификатом |
| `--db CONNINFO` | `host=/var/run/postgresql dbname=rockid` | строка подключения libpq |
| `--help`, `-h` | — | краткая справка |

Сервер слушает IPv6 и IPv4 на одном сокете. В лог на stderr пишутся время, уровень `info` или `error`, старт, адрес новой сессии и закрытие сокета. `SIGINT` и `SIGTERM` останавливают цикл `epoll` в течение примерно полусекунды. `SIGPIPE` игнорируется.

Каталог `build/` не предназначен для коммита. Пароль базы в строку подключения лучше не писать: при peer-аутентификации он не нужен.

Проверка после запуска:

```bash
python3 tools/client.py smoke --host 127.0.0.1 --port 7410
python3 tools/client.py repl --port 7410
```

`smoke` регистрирует двух случайных пользователей, создаёт личный диалог и группу, шлёт сообщения, читает историю и проверяет `auth.bind`. `repl` — ручной клиент одного соединения. Команды: `ping`, `signup <user> <pass> <name>`, `signin <user> <pass>`, `me`, `search <query>`, `dialogs`, `private <user_id>`, `send <dialog_id> <text>`, `history <dialog_id>`, `quit`.

Пример unit для systemd, если бинарник лежит в `/var/www/rockIDserver/build`:

```bash
sudo bash deploy/install.sh
```

Скрипт создаёт роль и базу, ставит `deploy/rockidserver.service` в systemd и включает автозапуск. Вручную тот же unit выглядит так:

```ini
[Unit]
Description=rockID messenger server
After=network-online.target postgresql.service
Wants=network-online.target

[Service]
Type=simple
User=oleg
Group=oleg
WorkingDirectory=/var/www/rockIDserver
ExecStart=/var/www/rockIDserver/build/rockidserver --port 7410 --db "host=/var/run/postgresql dbname=rockid"
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
```

Обновление уже запущенного сервера: пересобрать дерево `build/` и перезапустить процесс. Схема дополняется при старте.

```bash
cmake --build build -j
sudo systemctl restart rockidserver
```

В логе должны появиться строка `postgres`, `listening on [::]:7410` и `http on 7411`. `CREATE TABLE IF NOT EXISTS` при повторном старте пишет NOTICE «already exists, skipping».

TLS на порту мессенджера включается парой `--tls-cert` и `--tls-key`. HTTP мини-приложений остаётся открытым. Второй процесс с той же базой завершается сразу: соединение держит `pg_try_advisory_lock`.

## Архитектура

```text
клиент
  |  TCP :7410, кадр: uint32be длина + JSON
  |  HTTP :7411, мини-приложение и три метода бота
  v
Server (epoll, один поток)
  |-- Connection     буферы, TLS, zlib, разбор кадров
  |-- Hub            кто онлайн, push_user / push_many, очередь offline
  v
App                  методы API, печать секрета, эфир, файлы
  v
Database            PostgreSQL через libpq, advisory lock
```

Все колбэки `Hub` выполняются на потоке сервера. Отдельного пула воркеров нет: обработчик запроса не должен надолго блокироваться. PBKDF2 при регистрации и входе как раз блокирует этот поток (100 000 итераций SHA-256). Для каркаса это приемлемо; при росте нагрузки хеширование пароля стоит вынести из цикла `epoll`.

Поток одного запроса:

1. `epoll_wait` отмечает сокет читаемым.
2. `Connection` дочитывает данные до `EAGAIN` и режет буфер на кадры.
3. JSON разбирается, `App::handle` вызывает метод.
4. Ответ и пуши другим пользователям складываются в их выходные буферы.
5. Текущий сокет сразу дописывает свой буфер. Остальным `EPOLLOUT` выставляется через `epoll_ctl`, и они уходят на следующих итерациях цикла.
6. Медленный клиент, у которого выходной буфер превысил 8 МиБ, помечается к закрытию. Закрытие не делается внутри рассылки, чтобы не ломать обход списка сессий.

`Server` реализует `Hub`: `bind_user`, `unbind`, `push_user`, `push_many`, `online`. Смена присутствия с «никого» на «есть соединение» и обратно вызывает `App::broadcast_presence`.

## Каталоги

```text
include/rockid/     публичные заголовки слоёв
src/main.cpp        порт, строка подключения, сигналы
src/server.cpp      listen, epoll, онлайн-таблица
src/connection.cpp  кадры и сокет
src/json.cpp        разбор и сериализация JSON
src/app.cpp         методы протокола
src/app_features.cpp  файлы, эфир, боты, счета, истории
src/seal.cpp        печать секретного чата
src/database.cpp    схема и запросы
src/log.cpp         stderr-лог
third_party/libpq-fe.h
tools/client.py     smoke и repl
```

Зависимости между слоями односторонние. `Connection` знает `App`, `App` знает `Database` и `Hub`, `Server` знает сокеты. SQL не выходит за `database.cpp`.

## Протокол

Сокет — поток байтов. Кадр:

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      длина payload, uint32 BE                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     UTF-8 JSON, ровно length байт            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

Длина больше 1 МиБ или входной буфер больше 2 МиБ закрывают соединение без ответа. Старший бит длины (`0x80000000`) означает, что payload сжат zlib. Бит ставится только после `session.setOptions` с `compress: true` и только если сжатый кадр короче исходного. Распакованный размер по-прежнему не больше 1 МиБ. Клиент, который этот бит не понимает, соединение теряет: для него длина выглядит слишком большой.

Пустой объект и массив допустимы. Числа без дроби и экспоненты читаются как `int64`. `id` запроса — число или строка; если поля нет, в ответе будет `null`. Поле `params` можно опустить, тогда это `{}`.

Запрос:

```json
{"id": 1, "method": "messages.send", "params": {"dialog_id": 3, "text": "привет"}}
```

Успех:

```json
{"id": 1, "ok": true, "result": {"id": 10, "dialog_id": 3, "sender_id": 1, "text": "привет", "date": 1710000000}}
```

Ошибка:

```json
{"id": 1, "ok": false, "error": {"code": 403, "message": "NOT_MEMBER"}}
```

Пуш не имеет `id` и не ждёт ответа:

```json
{"update": "message.new", "data": {"id": 10, "dialog_id": 3, "sender_id": 1, "text": "привет", "date": 1710000000}}
```

`date` — unix-время в секундах. Строки в JSON — UTF-8. Разборщик понимает обычные escape-последовательности и `\uXXXX` для одного кода Unicode, без суррогатных пар. Управляющие символы в строке на входе должны быть экранированы.

Ключи объектов при сериализации сортируются: внутри используется `std::map`. Набор полей стабилен, порядок ключей тоже.

Кодек — подмножество JSON, достаточное для этого API. Это не общая библиотека и не место для произвольных расширений формата.

### Сессия на соединении

Без привязки пользователя доступны только `ping`, `auth.signUp`, `auth.signIn` и `auth.bind`. Остальные методы отвечают `401 UNAUTHORIZED`.

`auth.signUp` и `auth.signIn` создают токен, запоминают его на соединении и вызывают `bind_user`. `auth.bind` делает то же по уже выданному токену, поэтому второе устройство получает те же пуши. `auth.signOut` удаляет сессию из базы и снимает привязку. Сокет при этом не закрывается: дальше можно войти снова.

Токен — 32 случайных байта в hex (64 символа). В таблице `sessions` хранится SHA-256 этой строки. Потеря базы токенов разлогинивает всех; сами пароли при этом остаются.

Пароль: от 8 до 128 байт. Соль 16 байт, хеш 32 байта, 100 000 итераций PBKDF2-HMAC-SHA256. Если пользователя нет, сервер всё равно считает PBKDF2 на фиксированной соли, чтобы ответ «нет такого имени» не был заметно быстрее ответа «неверный пароль». Клиенту в обоих случаях возвращается `401 INVALID_CREDENTIALS`.

`username` — 3–32 символа, только `A-Z`, `a-z`, `0-9` и `_`. Уникальность в PostgreSQL проверяется по `lower(username)`. `display_name` — 1–64 байта без символов меньше 0x20.

## Методы

Ниже перечислены поля `params` и объект `result`. Обязательные поля помечены как обязательные.

### `ping`

Параметров нет.

```json
{"pong": true}
```

### `auth.signUp`

Обязательные: `username`, `password`, `display_name`.

```json
{
  "token": "hex-64",
  "user": {"id": 1, "username": "alice", "display_name": "Alice"}
}
```

`409 USERNAME_TAKEN`, если имя занято. `400 BAD_USERNAME`, `BAD_PASSWORD`, `BAD_DISPLAY_NAME`.

### `auth.signIn`

Обязательные: `username`, `password`. Ответ той же формы, что у регистрации. Каждый вход создаёт новую сессию и не гасит старые.

### `auth.bind`

Обязательный: `token`.

```json
{"user": {"id": 1, "username": "alice", "display_name": "Alice"}}
```

`401 INVALID_TOKEN`, если сессии нет.

### `auth.signOut`

Параметров нет, `result` — пустой объект. Повторный вызов без сессии на сокете даёт `401`, потому что метод не публичный.

### `users.getMe`

Текущий пользователь: `id`, `username`, `display_name`.

### `users.get`

Обязательный: `user_id`.

```json
{"id": 2, "username": "bob", "display_name": "Bob", "online": true}
```

`online` в ответе берётся из памяти процесса. Та же величина пишется в `users.online` и при каждом старте сбрасывается в `false`, поэтому после рестарта все офлайн, пока снова не подключатся. `404 USER_NOT_FOUND`.

### `users.search`

Обязательный: `query` (1–32 байта). Необязательный: `limit`, по умолчанию 20, максимум 50.

Ищется подстрока `username`, регистр ASCII не учитывается. Текущий пользователь в выдачу не входит. Символы `%`, `_` и `\` в запросе экранируются.

```json
{"users": [{"id": 2, "username": "bob", "display_name": "Bob", "online": false}]}
```

### `contacts.add`

Ровно одно из полей: `user_id` или `username`. Себя добавить нельзя (`400 BAD_CONTACT`). Контакт — односторонняя запись. Повторный вызов ничего не меняет и снова возвращает пользователя.

```json
{"id": 2, "username": "bob", "display_name": "Bob", "online": false}
```

### `contacts.get`

```json
{"contacts": [{"id": 2, "username": "bob", "display_name": "Bob", "online": true}]}
```

Сортировка по `username`.

### `dialogs.get`

Список диалогов зрителя, сначала те, где новее последнее сообщение.

```json
{
  "dialogs": [
    {
      "id": 3,
      "type": "private",
      "title": "",
      "member_count": 2,
      "unread": 1,
      "peer": {"id": 2, "username": "bob", "display_name": "Bob", "online": true},
      "last_message": {
        "id": 10,
        "dialog_id": 3,
        "sender_id": 2,
        "text": "привет",
        "date": 1710000000
      }
    }
  ]
}
```

У группы `type` равен `group`, `title` задан при создании, `peer` равен `null`. `unread` считает чужие сообщения с id строго больше курсора прочитанного. Свои сообщения непрочитанными не считаются. Если сообщений ещё не было, `last_message` равен `null`.

### `dialogs.createPrivate`

Обязательный: `user_id`. Диалог с самим собой запрещён (`400 BAD_PEER`). Пара хранится ключом `min(id):max(id)`, поэтому второй вызов с любой стороны возвращает тот же id.

В ответ приходит диалог глазами вызывающего. Собеседнику, если он онлайн, уходит пуш `dialog.new` с диалогом уже в его ракурсе: в `peer` будет вызывающий, а не он сам.

`404 USER_NOT_FOUND`.

### `dialogs.createGroup`

Обязательные: `title` (1–128 байт, без управляющих символов). Необязательный: `member_ids`, массив целых id.

Создатель становится `owner` и в массив можно его не класть. Повторы и отсутствующие id отбрасываются: неизвестный пользователь даёт `404 USER_NOT_FOUND`. Вместе с владельцем участников не больше 200 000 (`400 TOO_MANY_MEMBERS`). `dialogs.members` отдаёт страницами, по умолчанию 200.

Остальным участникам уходит `dialog.new`.

### `dialogs.addMember`

Обязательные: `dialog_id`, `user_id`. Только `owner` группы. Личный диалог отвечает `400 NOT_A_GROUP`. Чужой чат — `403 NOT_MEMBER`, участник без прав владельца — `403 NOT_OWNER`, нет диалога — `404 DIALOG_NOT_FOUND`.

Повторное добавление не ошибка:

```json
{
  "added": false,
  "dialog": { "id": 4, "type": "group", "title": "Weekend" }
}
```

Если человек добавлен впервые, ему уходит `dialog.new`, а остальным онлайн-участникам, кроме инициатора, ещё и `dialog.member`:

```json
{"update": "dialog.member", "data": {"dialog_id": 4, "user": {"id": 5, "username": "cara", "display_name": "Cara"}}}
```

### `dialogs.members`

Обязательный: `dialog_id`. Вызывающий должен быть участником.

```json
{
  "members": [
    {"id": 1, "username": "alice", "display_name": "Alice", "role": "owner", "online": true},
    {"id": 2, "username": "bob", "display_name": "Bob", "role": "member", "online": false}
  ]
}
```

В личном диалоге обе роли — `member`.

### `messages.send`

Обязательные: `dialog_id`, `text`. Текст от 1 до 4096 байт. Допустимы перевод строки и табуляция, остальные символы меньше 0x20 запрещены.

Ответ — объект сообщения (`id`, `dialog_id`, `sender_id`, `text`, `date`). Тем же объектом в `data` рассылается `message.new` всем соединениям участников, кроме сокета отправителя. Вторая сессия отправителя пуш получает. Каждому онлайн-получателю следом отправителю уходит `message.delivered`. Если получатель offline, `message.new` копится в `offline_pushes` (не больше 500 последних, при входе отдаётся до 200) и `message.delivered` приходит после `auth.bind`.

`403 NOT_MEMBER`, если пользователя нет в диалоге.

### `messages.getHistory`

Обязательный: `dialog_id`. Необязательные: `limit` (по умолчанию 50, максимум 100), `before_id` (вернуть сообщения со строго меньшим id; `0` или отсутствие поля означает «с конца»).

Сообщения в `result.messages` идут по возрастанию id.

```json
{"messages": [{"id": 9, "dialog_id": 3, "sender_id": 1, "text": "раз", "date": 1710000000}]}
```

### `messages.read`

Обязательные: `dialog_id`, `max_id` (неотрицательный). Курсор только растёт: более позднее меньшее значение не откатывает уже прочитанное.

```json
{"last_read_id": 10}
```

Отдельного пуша «прочитано» собеседнику пока нет.

## Пуши

| `update` | Когда | `data` |
| --- | --- | --- |
| `message.new` | после `messages.send` | объект сообщения |
| `message.delivered` | получатель онлайн или подтвердил `messages.ack` | `dialog_id`, `message_id`, `user_id` |
| `dialog.new` | создан видимый пользователю диалог или его добавили в группу | объект диалога в его ракурсе |
| `dialog.member` | в группу добавили участника | `dialog_id` и `user` |
| `presence` | первое соединение пользователя или закрытие последнего | `user_id`, `online` |
| `call.update` | старт, вход, выход, флаги и запись | объект эфира |
| `call.media` | кусок звука, видео или экрана | `call_id`, `user_id`, `kind`, `data` |

`presence` уходит тем, с кем есть общий диалог, а не всему списку контактов. Контакт без общего чата онлайн-статус по пушу не узнает; его можно спросить через `users.get`.

## Коды ошибок

Числовой `code` близок к HTTP, строка `message` стабильна и её стоит сравнивать в клиенте.

| code | message | Когда |
| --- | --- | --- |
| 400 | `BAD_JSON` | кадр не разобрался как JSON |
| 400 | `BAD_REQUEST` | корень не объект |
| 400 | `BAD_ID` | `id` не число, не строка и не null |
| 400 | `BAD_METHOD` | нет строкового `method` |
| 400 | `BAD_PARAMS` | `params` не объект или поле неверного типа |
| 400 | `BAD_USERNAME` | длина или алфавит имени |
| 400 | `BAD_PASSWORD` | короче 8 или длиннее 128 байт |
| 400 | `BAD_DISPLAY_NAME` | пустое, длиннее 64 или есть управляющий символ |
| 400 | `BAD_QUERY` | пустой или слишком длинный поиск |
| 400 | `BAD_CONTACT` | попытка добавить себя |
| 400 | `BAD_PEER` | личный диалог с собой |
| 400 | `BAD_TITLE` | заголовок группы |
| 400 | `BAD_MEMBERS` | `member_ids` не массив целых |
| 400 | `TOO_MANY_MEMBERS` | в группе больше 200 000 человек |
| 400 | `TOO_MANY_ACCOUNTS` | на `device_id` уже 3 аккаунта, или 4 с Premium |
| 400 | `PAYMENT_DETAILS_REJECTED` | в счёте есть поле карты, CVV или ключа кошелька |
| 400 | `BAD_KEY` | `keys.publish`: не 32 байта после base64 |
| 400 | `BAD_CHUNK` | кусок файла или эфира пустой либо длиннее лимита |
| 400 | `EDIT_FAILED` | `convert` или `ffmpeg` не собрали файл |
| 400 | `NOT_A_GROUP` | `addMember` на личном диалоге |
| 400 | `BAD_TEXT` | пустое, длиннее 4096 или запрещённый управляющий символ |
| 400 | `BAD_LIMIT` | `limit` вне допустимого диапазона |
| 400 | `BAD_BEFORE` | отрицательный `before_id` |
| 400 | `BAD_MAX_ID` | отрицательный `max_id` |
| 401 | `UNAUTHORIZED` | метод требует сессию |
| 401 | `INVALID_CREDENTIALS` | неизвестный пользователь или неверный пароль |
| 401 | `INVALID_TOKEN` | `auth.bind` не нашёл сессию |
| 403 | `NOT_MEMBER` | нет участия в диалоге |
| 403 | `NOT_OWNER` | действие только для владельца группы |
| 404 | `UNKNOWN_METHOD` | метод не реализован |
| 404 | `USER_NOT_FOUND` | нет пользователя |
| 404 | `DIALOG_NOT_FOUND` | нет диалога |
| 409 | `USERNAME_TAKEN` | имя занято |
| 500 | `INTERNAL` | неожиданная ошибка; подробность пишется в stderr, клиенту не отдаётся |

## База

Хранилище — PostgreSQL. Подключение задаётся строкой libpq, по умолчанию `host=/var/run/postgresql dbname=rockid`. Схема создаётся при старте операторами `CREATE TABLE IF NOT EXISTS`. Соль и хеш пароля лежат в `bytea`.

| Таблица | Содержимое |
| --- | --- |
| `users` | имя, хеш пароля, профиль, `online` |
| `sessions` | SHA-256 токена, устройство, вид локальной блокировки |
| `dialogs` | `private`, `secret`, `group`, `channel`, `saved` |
| `members` | пара `(dialog_id, user_id)`, роль и права |
| `messages` | текст, файл, таймер, перевод и расшифровка клиента |
| `read_state` | курсор `last_read_id` на пару пользователь–диалог |
| `contacts` | пара `(owner_id, peer_id)` |
| `files` | имя, mime, размер и готовность |
| `calls` / `call_members` | эфир и флаги микрофона, камеры, экрана |
| `offline_pushes` | пуши, которые некому было отдать |
| `device_accounts` | какие аккаунты входили с одним `device_id` |
| `user_keys` | публичный ключ X25519 |
| `mini_apps` | ссылка и HTML |
| `invoices` | сумма, статус, `provider_ref`, `pay_url` |

Сообщения можно править и удалять для всех. В личном чате срока нет.

## Дополнительные методы

Параметры сверх базовых `messages.send`: `kind` (`text`, `voice`, `video_note`, `file`, `photo`, `sticker`, `sponsor`), `reply_to`, `quote`, `file_id`, `silent`, `schedule_at`, `topic_id`, `entities` (JSON-массив, в том числе спойлеры).

| Метод | Назначение |
| --- | --- |
| `auth.checkCloudPassword` | второй шаг входа, если включён облачный пароль |
| `auth.botSignIn` | вход бота по токену из `bots.create` |
| `account.sessions` / `account.terminateSession` / `account.terminateOthers` | устройства и удалённый выход |
| `account.setLocalLock` | `kind`: `pin`, `bio` или `none` |
| `account.setPreferences` | JSON темы, фона и ночного режима, до 4 КБ |
| `users.updateProfile` / `users.setPrivacy` | телефон, био, фото; видимость `everybody`, `contacts`, `nobody` |
| `dialogs.createChannel` / `dialogs.join` / `dialogs.setDiscussion` | канал и чат комментариев |
| `dialogs.createGroup` с `forum: true`, `dialogs.createTopic` | темы форума |
| `dialogs.setAdmin` / `dialogs.kick` / `dialogs.ban` / `dialogs.leave` | модерация |
| `dialogs.archive` / `dialogs.mute` / `dialogs.pin` | архив, мьют (`until: -1` навсегда), закреп |
| `dialogs.getSaved` | Избранное |
| `messages.edit` / `messages.delete` / `messages.forward` / `messages.react` / `messages.view` | правка, удаление, пересылка, реакция, просмотры |
| `messages.reportScreenshot` / `stories.reportScreenshot` | клиент сообщает, что сделан скриншот |
| `stories.post` / `stories.get` / `stories.view` | истории на 6, 12, 24 или 48 часов |
| `stickers.createPack` / `stickers.add` / `stickers.list` | наборы стикеров, премиум-наборы скрыты без подписки |
| `folders.*` | папки чатов, 10 или 20 с Premium |
| `saved.createTab` / `saved.addToTab` / `saved.tag` | вкладки Избранного; теги только с Premium |

### `users.getMe`

К полям профиля добавляются `premium`, `premium_until`, `is_bot`, `cloud_password`, `preferences` и объект `limits`:

```json
{
  "limits": {
    "file_bytes": 2147483648,
    "folders": 10,
    "pins": 5,
    "channel_subscriptions": 500,
    "accounts": 3,
    "download_chunk": 65536,
    "upload_chunk": 393216
  }
}
```

С Premium: файл 4 ГБ, папки 20, закрепы 10, подписки 1000, аккаунты 4, скачивание и запись по 700 КБ. Premium на 30 дней включается счётом бота с `purpose: "premium"` после `payments.confirm`.

`auth.signUp` и `auth.signIn` принимают необязательный `device_id` (до 64 байт). Пока идентификатор не прислан, лимит аккаунтов не проверяется. Новый пользователь сверх лимита получает `400 TOO_MANY_ACCOUNTS`, уже входивший с этим `device_id` проходит снова.

### `account.setLocalLock`

Обязательный: `kind` — `pin`, `bio` или `none`. Поля с самим кодом или шаблоном биометрии сервер не читает и не хранит. В `account.sessions` у сессии остаётся только этот вид.

### `session.setOptions`

Необязательный: `compress` (по умолчанию `false`).

```json
{"compress": true}
```

Ответ на этот вызов ещё может уйти без сжатия. Следующие исходящие кадры соединения помечаются старшим битом длины, если zlib их укоротил.

### `messages.ack`

Обязательный: `message_id`. Участник диалога подтверждает доставку. Отправителю уходит `message.delivered`. Своё сообщение подтверждать некому.

### `keys.publish` и секретный чат

Обязательный: `public_key`, base64 ровно 32 байт (сырой X25519).

```json
{"bytes": 32}
```

`dialogs.createSecret` принимает `user_id` и необязательный `ttl_seconds` (0–604800). Если у каждого участника опубликован ключ и ключей не меньше двух, `messages.send` заменяет текст на JSON:

```json
{"e": 1, "seals": [{"user_id": 2, "box": "rk1.<epub>.<nonce>.<ciphertext>"}]}
```

`box` — эфемерный публичный ключ, nonce 12 байт и AES-256-GCM вместе с тегом. Ключ шифра — SHA-256 от общего секрета X25519. Эфемерный секретный ключ после печати уничтожается, сервер текст обратно не читает. Если ключа нет хотя бы у одного участника, в сообщение пишется тело, которое прислал клиент. Пересылка из секретного чата запрещена. Уведомление о скриншоте уходит только после `messages.reportScreenshot` или `stories.reportScreenshot`.

### Файлы

`files.create`: обязательные `name`, `mime`, `size`. В ответе есть `chunk_bytes`: 384 КБ или 700 КБ с Premium.

`files.write`: обязательные `file_id`, `offset`, `data` (base64). `offset` должен совпадать с уже принятым числом байт.

`files.finish` помечает файл готовым. `audio/*` перекодируется в `audio/ogg` (Opus), `video/*` — в `video/mp4`. Ошибка ffmpeg оставляет исходный файл.

`files.read`: `file_id`, необязательные `offset` и `length`. Длина чтения — до 64 КБ, с Premium до 700 КБ.

`files.edit` создаёт новый файл. Для `image/*`: `crop_w`, `crop_h`, `crop_x`, `crop_y`, `rotate` (0, 90, 180, 270), `brightness` и `contrast` от −100 до 100, `text` до 64 байт, область `spoiler_w` и `spoiler_h`. Для аудио и видео: `trim_start` и `trim_end` в секундах. Текст на картинке не проходит через оболочку.

Каталог по умолчанию — `/var/www/rockIDserver/data/files` на машине, где запущен процесс. Его меняет `--files`.

### Эфир

`calls.start` принимает `dialog_id`. Дальше `calls.join`, `calls.leave`, `calls.setState` (`muted`, `video`, `screen`) и `calls.record` (`recording`) работают по `call_id`. Ответ и пуш `call.update`:

```json
{
  "id": 7,
  "dialog_id": 3,
  "active": true,
  "recording": true,
  "peers": [{"user_id": 1, "muted": false, "video": false, "screen": false}]
}
```

`calls.media`: обязательные `call_id`, `kind` (`audio`, `video` или `screen`), `data` (base64, непустой, до 256 КБ). Участникам эфира, кроме отправителя, уходит `call.media`. В очередь offline этот пуш не кладётся. Пока `recording` истинно, куски дописываются в `call-<id>.ridm`: четыре байта `RIDM`, затем повторы из байта типа, длины uint32 BE и тела. `calls.record` с `recording: false` кладёт этот файл в Избранное как `application/x-rockid-call` с текстом «запись эфира». Если серверной записи нет, берётся `file_id` клиента. Сервер куски не смешивает и не отдаёт их как WebRTC.

### Перевод и расшифровка

`messages.setTranslation` сохраняет `text` и `lang`, которые прислал клиент. `messages.translate` возвращает эту пару и `"engine": "client"`. Своего перевода на сервере нет: пустое поле даёт `404 NO_TRANSLATION`.

`messages.setTranscript` доступен с Premium и только для `voice` и `video_note`. Сервер хранит присланный текст. Распознавания речи на машине нет.

### Боты, мини-приложения и счета

`bots.create`: обязательные `username` и `display_name`. В ответе у пользователя бота есть `token` из 64 hex-символов. `auth.botSignIn` входит по этому токену. `bots.getUpdates` забирает очередь.

Тот же токен работает по HTTP на порту `--http` (по умолчанию 7411):

```text
GET /bot<token>/getMe
GET /bot<token>/getUpdates
POST /bot<token>/sendMessage
```

Тело `sendMessage` — JSON с `chat_id` (это `dialog_id`) и `text`. Бот должен состоять в диалоге и иметь право писать. Ответ в форме Telegram: `{"ok": true, "result": ...}`. Запрос больше 256 КБ обрывается.

`apps.create`: `title`, `short_name`, `url` (`http://` или `https://`). `apps.setContent` записывает `html` владельца, до 200 КБ. Страница:

```text
GET /a/<short_name>
```

отдаёт сохранённый HTML. Окно WebView открывает клиент.

`payments.createInvoice` вызывает бот. Обязательные: `user_id`, `title`, `amount`, `currency` из трёх заглавных букв. Необязательные: `purpose`, `pay_url` (`http` или `https`, до 512 байт). Поля `card`, `pan`, `cvv`, `cvc`, `card_number`, `wallet_key`, `seed` дают `400 PAYMENT_DETAILS_REJECTED`. `payments.confirm` переводит счёт в `paid` по внешней строке `provider_ref`. `payments.cancel` ставит `cancelled`.

## Ограничения каркаса

- Распознавания речи и машинного перевода на сервере нет. Сохраняется текст, который прислал клиент.
- Карта, CVV и ключ кошелька не принимаются и не хранятся. Оплата идёт по `pay_url`, бот отмечает счёт внешней ссылкой `provider_ref`.
- Пин, Face ID и Touch ID на сервер не отправляются. `account.setLocalLock` помнит вид `pin`, `bio` или `none`.
- Скриншот виден серверу только как вызов `messages.reportScreenshot` или `stories.reportScreenshot`.
- Эфир ретранслирует куски и пишет их в файл. Смешивания дорожек и WebRTC нет.
- HTTP бота — это `getMe`, `sendMessage` и `getUpdates`. Полного Telegram Bot API нет.
- Рисование поверх кадра и анимированные стикеры собирает клиент. На сервер попадает готовый файл, а `files.edit` делает обрезку, цвет, текст и размытие.
- Кусок Premium — 700 КБ, потому что кадр ограничен 1 МиБ. Отдельного канала без лимита нет.
- Файлы лежат на диске машины, где запущен процесс.
- Один процесс и один поток `epoll`. Второй экземпляр на ту же базу не стартует.
- JSON-парсер покрывает этот API и не заменяет общую библиотеку.

Куда наращивать, не перемешивая слои:

- Смешивание эфира и постоянное хранилище файлов подключаются рядом с `calls.media` и `files.*`. `App` по-прежнему проверяет участие и рассылает пуш.
- Долгий PBKDF2, ffmpeg и запись больших вложений стоит уводить с потока `epoll`, оставляя порядок ответов по `id` на сокете.

## Минимальный обмен

Два клиента, далее кадры уже без обёртки длины.

Алиса:

```json
{"id": 1, "method": "auth.signUp", "params": {"username": "alice", "password": "password12", "display_name": "Alice"}}
```

Боб проходит тот же метод со своим именем. Алиса, зная `user.id` Боба:

```json
{"id": 2, "method": "dialogs.createPrivate", "params": {"user_id": 2}}
{"id": 3, "method": "messages.send", "params": {"dialog_id": 1, "text": "привет"}}
```

Боб на своём сокете получает сначала `dialog.new`, затем `message.new`, после чего может вызвать `messages.getHistory` и `messages.read`.
