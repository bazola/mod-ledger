-- mod-ledger: append-only history of what bots and real players do together.
-- Contract for later phases (episodes, relationships, retrieval). Changing a
-- column after rows accumulate is a migration, not an edit to this file.

CREATE TABLE IF NOT EXISTS `ledger_chat` (
  `id` BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  `ts` DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),  -- server-local time (CDT) at insert
  `speaker_guid` INT UNSIGNED NOT NULL,
  `speaker_name` VARCHAR(24) NOT NULL,
  `speaker_is_bot` TINYINT NOT NULL,
  `listener_guid` INT UNSIGNED NULL,      -- whisper target / nearest real player (say, yell, emote)
  `chat_type` TINYINT NOT NULL,           -- core ChatMsg: 1 say, 2 party, 3 raid, 4 guild, 5 officer,
                                          -- 6 yell, 7 whisper, 10 emote, 17 channel, 39 raid leader,
                                          -- 40 raid warning, 44 bg, 45 bg leader, 51 party leader
  `channel` VARCHAR(32) NULL,             -- channel name for chat_type 17, else NULL
  `zone_id` INT UNSIGNED NOT NULL,
  `map_id` INT UNSIGNED NOT NULL,
  `text` VARCHAR(512) NOT NULL,
  KEY `by_pair_time` (`speaker_guid`,`listener_guid`,`ts`),
  KEY `by_time` (`ts`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `ledger_event` (
  `id` BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  `ts` DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),  -- server-local time (CDT) at insert
  `actor_guid` INT UNSIGNED NOT NULL,
  `actor_is_bot` TINYINT NOT NULL,
  `event_type` VARCHAR(32) NOT NULL,      -- kill, death, pvp_kill, level_up, quest_complete, loot_item,
                                          -- duel_won, duel_lost, group_join, group_leave, zone_change,
                                          -- guild_invite, guild_join, guild_rank, guild_leave,
                                          -- guild_found, guild_disband
  `subject_guid` INT UNSIGNED NULL,       -- other character (victim, killer, opponent, group leader,
                                          -- guild invitee, who changed the rank or removed the member)
  `zone_id` INT UNSIGNED NOT NULL,
  `map_id` INT UNSIGNED NOT NULL,
  `detail` VARCHAR(255) NULL,             -- JSON: creature entry, quest id, item id, ...
  KEY `by_actor_time` (`actor_guid`,`ts`),
  KEY `by_type_time` (`event_type`,`ts`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
