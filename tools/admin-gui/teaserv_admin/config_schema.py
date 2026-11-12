"""Which settings of system/config.json the Settings window shows as labelled fields, grouped by topic.

Every key that is not listed here stays reachable under "Other settings" as raw text. `apply` says when a change takes
effect: "reload" (the server's `reload config` command is enough) or "restart" (the server reads the key only when it
starts). The split follows DataIndex::load_config_early: ports and listen addresses are read once.
"""

import ipaddress
import re

RELOAD = "reload"
RESTART = "restart"

EVENTS = ["none", "xmas", "val", "easter", "hallo", "sonic", "newyear", "summer", "white", "wedding", "fall",
          "s-spring", "s-summer", "spring"]
EVENT_LABELS = {
    "none": "None", "xmas": "Christmas", "val": "Valentine's Day", "easter": "Easter", "hallo": "Halloween",
    "sonic": "Sonic", "newyear": "New Year", "summer": "Summer", "white": "White Day", "wedding": "Wedding",
    "fall": "Fall", "s-spring": "Spring (school)", "s-summer": "Summer (school)", "spring": "Spring",
}
LOBBY_NAMES = ["Lobby %d" % n for n in range(1, 16)] + ["Card lobby C%d (Episode 3)" % n for n in range(1, 6)]

LOG_LEVELS = ["DEBUG", "INFO", "WARNING", "ERROR", "DISABLED"]
LOG_STREAMS = [
    ("Clients", "Players", "Events of one player's connection or game state."),
    ("Lobbies", "Lobbies and games", "Lobbies and games created and deleted; Episode 3 battle debug text."),
    ("GameServer", "Connections", "Players connecting to and leaving the game server."),
    ("PlayerData", "Player data", "Loading and saving of player and account files."),
    ("CommandData", "Network commands (raw data)", "Every command sent and received. Very noisy at INFO."),
    ("ChannelExceptions", "Connection errors", "Players who disconnect unexpectedly and other network events."),
    ("Config", "Start-up and configuration", "Server-wide events, mostly while starting."),
    ("ProxyServer", "Proxy server", "Proxy sessions to other servers."),
    ("DNSServer", "DNS server", "DNS queries the server does not answer."),
    ("IPStackSimulator", "Dolphin / modem connections", "Clients that connect through the IP or PPP simulator."),
    ("PatchFileIndex", "Patch files", "Indexing of PC and Blue Burst patch files."),
    ("ClientFunctionIndex", "Client functions", "Building of client patches at start-up."),
    ("StaticGameData", "Game data", "Loading of game data files."),
    ("Replay", "Replays", "Session-log replays (testing only)."),
]

EP3_SONGS = [
    "Let the winds blow - Theme of PSO Episode3", "Gate", "Tune", "Code", "NEW LIFES", "RIDE ON", "ADVICES",
    "Morgue PART1", "Unguis lapis", "Via Tubus", "Tower of Caelum", "Mortis Fons", "Lupus Silva PART1",
    "Lupus Silva PART2", "Molae Venti", "Tener Sinus", "The whole new world - PSO OPENING THEME",
    "World with me - PSO EP2 ENDING THEME", "Can still see the light - PSO ENDING THEME", "Day dawns",
    "Nebula Montana PART1", "Nebula Montana PART2", "Morgue PART2", "Dolor Odor", "Ravum Aedes Sacra",
    "IDOLA the strange fruits", "Cyber", "Special Relaxies", "Let the winds blow -Remix Version-", "Leavin flow",
    "Rose Confession", "Day light", "Versus1 -Tricktrack-", "Versus2 -A longing to ancient times-",
    "Burning Hearts - Burning Ranger", "Wedding March - SAMBA de AMIGO", "VAMOS A CARNAVAL - SAMBA de AMIGO",
    "dreams dreams - Nights", "dreams dreams (kids ver) - Nights", "CHANT THIS CHARM - BILLY HATCHER",
    "Let Mom Sleep - Jet Grind Radio", "THE CONCEPT OF LOVE - Jet Grind Radio Future",
    "Where is smiley? - NEW ROOMMANIA", "Buggie Running Beeps 01 - Rez", "Skies of Arcadia Opening Theme",
    "Shinobi :boutan - Shinobi", "Tsuioku - Panzer Dragoon ZWEI", "Sona mi areru ec sancitu - AZEL Panzer Dragoon RPG",
    "ANU ORTA VENIYA - Panzer Dragoon ORTA", "LET'S GO AWAY - DAYTONA", "MAIN THEME - SPACE HARRIER",
    "OPA-OPA! - Fantasy Zone",
]

EP3_FLAGS = [
    (0x0001, "Turn off deck verification completely"),
    (0x0002, "Do not check how many copies of a card the player owns"),
    (0x0004, "Allow D1 and D2 rank cards in battle"),
    (0x0008, "Turn off battle time limits (overall and per phase)"),
    (0x0010, "Show debug messages in Episode 3 games and battles"),
    (0x0040, "Record battles (players can save with $saverec)"),
    (0x0080, "Turn off command masking during battles"),
    (0x0100, "Turn off interference (COMs helping each other)"),
    (0x0200, "Allow interference when no player is a COM"),
]


class Field:
    def __init__(self, path, label, kind, help="", apply=RELOAD, optional=False, default=None, **options):
        self.path = tuple(path) if isinstance(path, (list, tuple)) else (path,)
        self.label = label
        self.kind = kind  # text, multiline, int, float, bool, choice, seconds, flags, lines, intlist, events
        self.help = help
        self.apply = apply
        self.optional = optional  # the key may be absent; an empty field removes it
        self.default = default  # value the server uses when the key is absent (shown, not written)
        self.options = options

    @property
    def name(self):
        return ".".join(self.path)

    @property
    def report_name(self):
        """Name for messages: fields inside a group say which group."""
        if self.path[0] == "LogLevels" and len(self.path) > 1:
            return "Log level of \"%s\"" % self.label
        return self.label

    # ----- does a value from the file fit this field?

    def accepts(self, value):
        kind = self.kind
        if kind in ("text", "multiline"):
            return isinstance(value, str)
        if kind in ("int", "seconds", "flags"):
            return isinstance(value, int) and not isinstance(value, bool)
        if kind == "float":
            return isinstance(value, (int, float)) and not isinstance(value, bool)
        if kind == "bool":
            return isinstance(value, bool)
        if kind == "choice":
            return any(value == choice for choice, _ in self.options["choices"])
        if kind == "lines":
            return isinstance(value, list) and all(
                isinstance(v, (str, int)) and not isinstance(v, bool) for v in value)
        if kind == "intlist":
            return isinstance(value, list) and all(isinstance(v, int) and not isinstance(v, bool) for v in value)
        if kind == "events":
            return isinstance(value, list) and len(value) == len(LOBBY_NAMES) and all(v in EVENTS for v in value)
        return False

    # ----- checks on a value the admin entered; returns the value to write or raises ValueError

    def check(self, value):
        kind, opt = self.kind, self.options
        if kind in ("text", "multiline"):
            limit = opt.get("max_length")
            if limit and len(value) > limit:
                raise ValueError("At most %d characters (now %d)." % (limit, len(value)))
            pattern = opt.get("pattern")
            if pattern and not re.match(pattern, value):
                raise ValueError(opt.get("pattern_help", "The value has the wrong form."))
            if opt.get("address") and not valid_address(value):
                raise ValueError("Enter an IPv4 address such as 192.168.1.20, or a network interface name.")
            return value
        if kind in ("int", "seconds", "float"):
            low, high = opt.get("min"), opt.get("max")
            if low is not None and value < low:
                raise ValueError("The smallest allowed value is %s." % low)
            if high is not None and value > high:
                raise ValueError("The largest allowed value is %s." % high)
            if kind == "seconds":
                return int(round(value * 1000000))
            return value
        if kind == "lines":
            item_check = opt.get("item")
            out = []
            for number, item in enumerate(value, 1):
                try:
                    out.append(item_check(item) if item_check else item)
                except ValueError as e:
                    raise ValueError("Line %d: %s" % (number, e))
            return out
        if kind == "intlist":
            count = opt.get("count")
            if count and len(value) != count:
                raise ValueError("Enter exactly %d numbers separated by commas." % count)
            low, high = opt.get("min"), opt.get("max")
            for v in value:
                if (low is not None and v < low) or (high is not None and v > high):
                    raise ValueError("Each number must be between %s and %s." % (low, high))
            if opt.get("ascending") and list(value) != sorted(value):
                raise ValueError("The numbers must go from small to large.")
            return list(value)
        return value


def valid_address(text):
    if not text or " " in text:
        return False
    if re.match(r"^[0-9.]+$", text):
        try:
            ipaddress.IPv4Address(text)
            return True
        except ValueError:
            return False
    return bool(re.match(r"^[A-Za-z0-9_.:{}\-]+$", text))


def listen_item(item):
    """A port number, or "address:port" (not on Windows: the server skips text entries there)."""
    text = str(item).strip()
    if re.match(r"^\d+$", text):
        port = int(text)
        if not 1 <= port <= 65535:
            raise ValueError("a port number is between 1 and 65535")
        return port
    match = re.match(r"^(.+):(\d+)$", text)
    if match and 1 <= int(match.group(2)) <= 65535:
        return text
    raise ValueError("enter a port number (for example 5059) or address:port (for example 127.0.0.1:5059)")


def banned_range_item(item):
    text = str(item).strip()
    try:
        if "/" in text:
            ipaddress.IPv4Network(text, strict=False)
        else:
            ipaddress.IPv4Address(text)
    except ValueError:
        raise ValueError("enter an IPv4 address (1.2.3.4) or a range (1.2.3.0/24)")
    return text


TOPICS = [
    ("General", "Name and messages players see.", [
        Field("ServerName", "Server name", "text", max_length=16,
              help="Shown in the upper-right corner of the screen in lobbies. At most 16 characters."),
        Field("WelcomeMessage", "Welcome message", "multiline",
              help="Shown to GameCube players when they connect. Leave empty for no message. "
                   "PSO colour codes such as $C6 work."),
        Field("Episode3MenuSong", "Episode 3 menu music", "choice", optional=True,
              choices=[(n, "%d: %s" % (n, name)) for n, name in enumerate(EP3_SONGS)],
              help="Song Episode 3 players hear at the server's main menu. \"(not set)\" keeps the game's own music."),
        Field("EnableChatCommands", "Chat commands for all players", "bool",
              help="Off: only accounts with the ALWAYS_ENABLE_CHAT_COMMANDS flag can use $ commands."),
        Field("ChatCommandSentinel", "Chat command prefix", "text", optional=True, max_length=1, default="$",
              help="One character that starts a chat command. Empty: the default, $."),
        Field("BackupCharacterSlots", "Backup character slots per account", "int", min=0, max=1000,
              help="Used by $savechar, $loadchar and $checkchar. A small number such as 16 is recommended."),
    ]),
    ("Network", "Addresses and ports. Port changes need a server restart.", [
        Field("LocalAddress", "Local address", "text", address=True,
              help="Address players on your own network connect to: this PC's IPv4 address (for example "
                   "192.168.1.20) or a network interface name. Use 127.0.0.1 for a server only you use."),
        Field("ExternalAddress", "External address", "text", address=True,
              help="Address players on the Internet connect to: usually your router's public IPv4 address."),
        Field("DNSServerPort", "DNS server port", "int", apply=RESTART, min=0, max=65535,
              help="Port for the built-in DNS server. Consoles use 53. 0 turns the DNS server off."),
        Field("HTTPListen", "HTTP status interface (ports)", "lines", apply=RESTART, item=listen_item,
              help="One port per line. Private interface for web pages or bots; leave empty unless you use it. "
                   "Never open these ports to the Internet."),
        Field("IPStackListen", "Dolphin (IP stack) ports", "lines", apply=RESTART, item=listen_item,
              help="One port per line, for GameCube games in the Dolphin emulator (tapserver BBA)."),
        Field("PPPStackListen", "PPP stack ports", "lines", apply=RESTART, item=listen_item,
              help="One port per line, for modem (PPP) connections through a tapserver."),
        Field("PPPRawListen", "Raw PPP ports (Devolution)", "lines", apply=RESTART, item=listen_item,
              help="One port per line, for GameCube games on a Wii with Devolution."),
        Field("BannedIPV4Ranges", "Banned IP addresses", "lines", item=banned_range_item,
              help="One address (1.2.3.4) or range (1.2.3.0/24) per line. Players from these addresses are "
                   "disconnected at once."),
        Field("ClientPingInterval", "Ping idle players after (seconds)", "seconds", min=1, max=3600,
              help="How long a player may send nothing before the server checks that they are still there."),
        Field("ClientIdleTimeout", "Disconnect silent players after (seconds)", "seconds", min=2, max=86400,
              help="Must be longer than the ping time, so that a player can answer the ping."),
    ]),
    ("Accounts", "Who may log in.", [
        Field("AllowUnregisteredUsers", "Let anyone log in", "bool",
              help="On: a new account is made for each unknown player. Off: only accounts you created can log in."),
        Field("AllowSameAccountConcurrentLogins", "Allow several logins on one account", "bool",
              help="Off: a new login disconnects the older session of the same account."),
        Field("UseTemporaryAccountsForPrototypes", "Temporary accounts for prototype versions", "bool",
              help="Prototype (NTE) game versions get accounts that are not saved."),
        Field("AllowPCNTE", "Allow PSO PC NTE players", "bool",
              help="This version cannot identify the player, so these players cannot be banned by account."),
    ]),
    ("Lobby events", "Holiday decorations.", [
        Field("MenuEvent", "Main menu event", "choice", choices=[(e, EVENT_LABELS[e]) for e in EVENTS],
              help="The holiday event players see at the main menu, and in overflow lobbies."),
        Field("LobbyEvents", "Event in each lobby", "events",
              help="Holiday event of each of the 20 lobbies. Players can change it for one lobby with $event."),
    ]),
    ("Episode 3", "Card battle rules, Meseta and the card auction.", [
        Field("Episode3BehaviorFlags", "Battle rules", "flags", flags=EP3_FLAGS,
              help="With every box off, battles behave as on the original Sega servers."),
        Field("Episode3InfiniteMeseta", "Unlimited Meseta for everyone", "bool",
              help="Makes the jukebox and the card shop free."),
        Field("Episode3JukeboxIsFree", "Free jukebox", "bool",
              help="The jukebox takes no Meseta. The game still needs the player to hold 100 Meseta."),
        Field("Episode3DefeatPlayerMeseta", "Meseta for beating a player (tournament rounds 1-5)", "intlist",
              count=5, min=0, max=1000000),
        Field("Episode3DefeatCOMMeseta", "Meseta for beating a COM (tournament rounds 1-5)", "intlist",
              count=5, min=0, max=1000000),
        Field("Episode3FinalRoundMesetaBonus", "Extra Meseta for winning the final", "int", min=0, max=1000000),
        Field("CardAuctionPoints", "Card auction: points per player", "int", min=0, max=65535),
        Field("CardAuctionSize", "Card auction: number of cards (smallest, largest)", "intlist",
              count=2, min=1, max=20, ascending=True,
              help="The server picks a number of cards in this range for each auction."),
    ]),
    ("Games", "Rules for games (rooms).", [
        Field("CheatModeBehavior", "Cheat mode", "choice", choices=[
            ("Off", "Off - cheats cannot be used"),
            ("OffByDefault", "Off by default - $cheat turns it on in a game"),
            ("OnByDefault", "On by default - $cheat turns it off in a game"),
        ]),
        Field("PersistentGameIdleTimeout", "Delete empty persistent games after (seconds)", "seconds",
              min=0, max=31536000,
              help="For games kept open with $persist. 0: they are never deleted."),
    ]),
    ("Logging", "How much the server writes to its log.", [
        Field(("LogLevels", key), label, "choice", choices=[(level, level) for level in LOG_LEVELS], help=help_text)
        for key, label, help_text in LOG_STREAMS
    ] + [
        Field("HideDownloadCommands", "Hide large download commands in the log", "bool"),
        Field("CensorCredentials", "Hide passwords and access keys in the log", "bool",
              help="Leave on unless you are tracking down a login problem."),
    ]),
    ("Statistics", "What the Statistics window records.", [
        Field("TeaservStatsInterval", "Record statistics every (seconds)", "int", apply=RESTART, optional=True,
              default=60, min=0, max=86400,
              help="The server adds one line to system/stats each interval. 0: recording is off. "
                   "Empty: the default, 60."),
    ]),
]


def all_fields():
    for _, _, fields in TOPICS:
        for field in fields:
            yield field


def form_top_level_keys():
    return {field.path[0] for field in all_fields()}
