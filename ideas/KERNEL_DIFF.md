# Diff ядер 3.10 vs 6.6 — SM0 I2C для PIC

## Главный вывод
SM0_CFG=0x00 на ОБОИХ ядрах. Разница НЕ в регистрах SM0.

## Ключевые отличия

### 1. I2C архитектура
| | Kernel 3.10 (stock) | Kernel 6.6 (OpenWrt) |
|--|---------------------|---------------------|
| I2C режим | SM0 Auto mode | NEW Manual mode (i2c-mt7621) |
| SM0_CFG2 | 0x01 (auto) | 0x00 (manual) при kernel probe |
| SM0_CFG | 0x00 (READ-ONLY) | 0x00 (READ-ONLY) |
| N_CTL0 | 0x8064800E (hw-mod) | 0x01F3800F → 0x8064800E |
| PIC driver | built-in (AlmondPic2) | наш lcd_drv.ko |
| Мьютекс SM0 | Единственный driver | i2c-mt7621 + lcd_drv конфликт! |

### 2. КРИТИЧЕСКАЯ РАЗНИЦА: конфликт доступа к SM0
**Стоковое ядро 3.10:**
- ОДИН встроенный driver (AlmondPic2) владеет SM0 целиком
- Touch (SX8650) и PIC через тот же driver с внутренним mutex
- Нет конфликта — все SM0 операции сериализованы

**Наше ядро 6.6:**
- **i2c-mt7621 kernel driver** владеет SM0 (manual mode)
- MT7530 PHY polling через kernel i2c каждые ~секунду
- **lcd_drv.ko** пытается использовать SM0 auto mode одновременно
- **КОНФЛИКТ**: kernel driver может прервать нашу PIC транзакцию!

### 3. Тайминги write (из IDA дизассемблирования)
| | Kernel 3.10 | Наш lcd_drv |
|--|------------|------------|
| Между write bytes | udelay(5000) = **5ms!** | udelay(1000) |
| Poll wait | POLLSTA bit 1 | POLLSTA bit 1 |
| Read byte delay | udelay(10) | udelay(10) |
| Write→Read delay | 500ms (worker loop) | 500ms |

### 4. Worker loop протокол
**Stock (из SM0 poll capture):**
```
Write: SM0_DATA=0x2A, STRT=1, STAT=0 (1 byte write)
Read:  SM0_DATA=0x2A, STRT=5, STAT=1 (6 bytes read)
Период: ~25 секунд между read циклами
```

**Наш lcd_drv (все варианты проверены):**
- SM0 auto write(3) + read(8) → ff ff ff...
- SM0 auto write(1) + read(6) → ff ff ff...
- bit-bang write(3) + read(8) → aa 54 a8...
- bit-bang write(1) + read(6) → aa 54 a8...
- NEW manual write + read → aa 54 a8...
- kernel i2c_transfer → NACK

### 5. SM0 регистры на СТОКОВОМ ядре (live dump через restdebug)
```
SM0_CFG  (0x900) = 0x00000000  ← READ-ONLY и на стоке!
SM0_DATA (0x908) = 0x00000048  ← SX8650 addr (idle)
SM0_STAT (0x91C) = 0x00000001  ← read mode
SM0_STRT (0x920) = 0x00000001
SM0_CFG2 (0x928) = 0x00000001  ← AUTO mode!
N_CTL0   (0x940) = 0x8064800E
```

## Гипотеза: конфликт i2c-mt7621

На стоковом ядре — один driver владеет SM0. На OpenWrt — kernel i2c-mt7621 driver
делает периодические транзакции (MT7530 PHY, SX8650 probe, etc) через SM0,
используя NEW manual mode. Наш lcd_drv переключает SM0 в auto mode для PIC read,
но kernel driver может в любой момент:
1. Переключить SM0_CFG2 обратно в manual mode
2. Изменить N_CTL0/N_CTL1
3. Сбросить SM0 state машину

Это объясняет почему ВСЕ наши read методы дают bus noise — SM0 транзакция
прерывается kernel driver'ом.

## Следующий шаг
**Использовать i2c-mt7621 kernel API** (i2c_transfer) для PIC read.
Если kernel driver делает I2C read правильно (через NEW manual mode с clock stretching),
то i2c_transfer для PIC должен работать. Ранее ret=-6 (ENXIO) = PIC NACK.
Но может PIC NACKает потому что kernel driver использует слишком быстрый clock
для PIC (100kHz vs PIC's clock stretching need).

**Или**: собрать i2c-mt7621 с пониженной скоростью (10kHz?) и retry logic.
