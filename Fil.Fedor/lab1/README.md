# Лабораторная №1. Вариант 20

Отслеживание файловых событий в заданных каталогах с записью в отдельный журнал.

## Сборка в Linux

```sh
./build.sh
```

Скрипт использует CMake, создаёт `lab1` и удаляет промежуточные файлы.

## Настройка и запуск

В `lab1.conf` укажите существующие каталоги:

```ini
directory = data/watched
```

Относительные пути отсчитываются от папки конфига.

```sh
mkdir -p data/watched
sudo cp rsyslog/30-fil-fedor-disk-monitor.conf /etc/rsyslog.d/
sudo systemctl restart rsyslog
./lab1
```

## Управление

```sh
kill -HUP "$(cat /tmp/fil-fedor-lab1-$(id -u).pid)"   # перечитать конфиг
kill -TERM "$(cat /tmp/fil-fedor-lab1-$(id -u).pid)"  # остановить
sudo tail -f /var/log/fil-fedor-disk-monitor.log
```

Повторный запуск заменяет предыдущий экземпляр. Служебные сообщения:
`journalctl -t fil-fedor-lab1`.

## Тест

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
