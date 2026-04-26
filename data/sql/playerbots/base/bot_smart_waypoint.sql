DROP TABLE IF EXISTS `bot_smart_waypoint`;
CREATE TABLE `bot_smart_waypoint` (
  `id` int NOT NULL AUTO_INCREMENT,
  `pos_x` int DEFAULT '0',
  `pos_y` int DEFAULT '0',
  `pos_z` int DEFAULT '0',
  `smartId` int DEFAULT '0',
  `type` int DEFAULT '0',
  `orderNum` int DEFAULT '0',
  `groupId` int DEFAULT '0',
  `param1` int DEFAULT '0',
  `param2` int DEFAULT '0',
  `param3` int DEFAULT '0',
  PRIMARY KEY (`id`),
  KEY `smart_index` (`smartId`) USING BTREE,
  KEY `smart_group_order_index` (`smartId`, `groupId`, `orderNum`) USING BTREE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci ROW_FORMAT=DYNAMIC;

