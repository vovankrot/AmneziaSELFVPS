# SELFVPS

VPN-клиент для собственного VPS на базе [AmneziaVPN](https://github.com/amnezia-vpn/amnezia-client).
Неофициальный форк с доработками протоколов, интерфейса и управления сетью Windows.

![Главный экран SELFVPS](docs/screenshots/home.png)

## Возможности

- AmneziaWG, XRay, Hysteria 2 и AnyTLS.
- Управление сервером, протоколами и клиентскими конфигурациями.
- Раздельное туннелирование по приложениям и сайтам для поддерживаемых протоколов.
- Настройки DNS, Kill Switch и доступа к локальной сети.
- Обновление Hysteria на VPS с проверкой файла, журналом выполнения и откатом при ошибке.
- Клиент Windows, сборка Android и собственный установщик Windows.
- Проверка обновлений из GitHub Releases с проверкой целостности и подтверждением установки.

Работоспособность зависит от сети, сервера и выбранного протокола. Изменения, результаты проверок и известные ограничения описаны в [релизе 5.0.0.12](docs/releases/5.0.0.12.md).

## Скачать

Готовые пакеты доступны в [GitHub Releases](https://github.com/vovankrot/AmneziaSELFVPS/releases).
Исходники установщика и Build Studio находятся в `installer/` и `launcher/`.

## Собрать

Клонируйте проект вместе с подмодулями:

```sh
git clone --recurse-submodules https://github.com/vovankrot/AmneziaSELFVPS.git
cd AmneziaSELFVPS
```

После установки зависимостей из [инструкции сборки](SETUP.md):

```powershell
# Windows
.\build_installer.ps1 -SkipAndroidApk
# Android
.\build_android.ps1
```

Готовые EXE/APK этих сценариев находятся в `dist/`. Build Studio можно запускать из дерева проекта; он использует те же сценарии сборки.

## Разработка

- [Зависимости и сборка](SETUP.md)
- [Структура проекта и очистка](docs/repository-layout.md)
- [Правила участия](CONTRIBUTING.md)
- [Проверки интерфейса](tests/ui-regressions/README.md)
- [Проверки обработки отказов драйвера](tests/driver-regressions/README.md)
- [Зависимости форка](deploy/prebuilt-selfvps/README.md)

Для просмотра накопившихся временных файлов:

```powershell
.\tools\clean-workspace.ps1
```

Команда без параметров ничего не удаляет. Готовые релизы, локальные настройки и исходники сохраняются при очистке.

## Лицензия и авторство

Основной проект разработан командой Amnezia. Этот репозиторий содержит изменения поверх AmneziaVPN и не является официальной сборкой команды.

См. [LICENSE](LICENSE), [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) и лицензии зависимостей.
