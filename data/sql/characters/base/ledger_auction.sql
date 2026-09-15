-- mod-ledger: the auction houses' history (custom wow plans/17 §3.E.1). One row per listing, sale and expiry,
-- whoever the seller or buyer is; the market service classifies them (merchant, bot, player) by account.

CREATE TABLE IF NOT EXISTS `ledger_auction` (
  `id` BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  `ts` DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),  -- server-local time (CDT) at insert
  `event` VARCHAR(8) NOT NULL,            -- list, sale (won bid or buyout), expire (no bid)
  `auction_id` INT UNSIGNED NOT NULL,
  `house_id` TINYINT UNSIGNED NOT NULL,   -- 2 Alliance, 6 Horde, 7 neutral
  `item_entry` INT UNSIGNED NOT NULL,
  `item_count` INT UNSIGNED NOT NULL,
  `start_bid` INT UNSIGNED NOT NULL,
  `buyout` INT UNSIGNED NOT NULL,         -- 0 when the listing has none
  `price` INT UNSIGNED NOT NULL,          -- copper paid for the whole stack on a sale, else 0
  `owner_guid` INT UNSIGNED NOT NULL,
  `bidder_guid` INT UNSIGNED NULL,        -- the buyer on a sale
  KEY `by_time` (`ts`),
  KEY `by_item_time` (`item_entry`,`ts`),
  KEY `by_event_time` (`event`,`ts`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
