# RPC Messenger

Локальный чат на C++ для двух и более терминалов одного пользователя Linux.
Первый терминал открывает комнату и участвует в переписке. Закрытие первого
терминала завершает сеанс у всех.

## Сборка

```sh
# Fedora
sudo dnf install gcc-c++ make cmake capnproto capnproto-devel python3
# Debian / Ubuntu
sudo apt install g++ make cmake capnproto libcapnp-dev python3
make
```

## Запуск

```sh
./build/messenger listen --nick Анна    # первый терминал
./build/messenger connect --nick Илья   # второй терминал
./build/messenger connect --nick Оля    # ещё один терминал
```

Enter отправляет сообщение. `/quit` или Ctrl+D завершает работу.
Без `--nick` имя назначается автоматически. С `--room ИМЯ` все участники
указывают одно имя комнаты. `--color=auto|always|never` управляет цветом меток.

Cap’n Proto RPC принимает сообщения и вызывает обратный метод у участников.
Именованные каналы размещаются в приватном каталоге в `$XDG_RUNTIME_DIR`,
а при его отсутствии или недоступности — в `$TMPDIR` или `/tmp`.

`make test` запускает интеграционные проверки.
