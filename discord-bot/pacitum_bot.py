"""Pacitum, the Discord bot for the Pacitum Browser community.

- Release notes: posts each new GitHub release to the release-note channel (checked every few minutes),
  and /release shows the notes of any version.
- Bug reports: /bug opens a form, posts the report to the bug-report channel with a discussion thread,
  and (with a GitHub token) opens a GitHub issue for it.
- Suggestions: /suggest opens a form, posts the idea to the suggestion channel with 👍 / 👎 votes and a thread.
- Staff (Manage Messages) set a status on reports and suggestions with the buttons under each post.

Settings come from environment variables (or a .env file next to this script): see README.md.
"""
from __future__ import annotations

import json
import logging
import os
import pathlib
import re

import aiohttp
import discord
from discord import app_commands
from discord.ext import tasks

HERE = pathlib.Path(__file__).resolve().parent
log = logging.getLogger("pacitum-bot")


def load_env_file() -> None:
    env = HERE / ".env"
    if not env.exists():
        return
    for line in env.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))


load_env_file()


def env_int(name: str) -> int | None:
    value = os.environ.get(name, "").strip()
    return int(value) if value.isdigit() else None


TOKEN = os.environ.get("DISCORD_TOKEN", "").strip()
GUILD_ID = env_int("GUILD_ID")
RELEASE_CHANNEL_ID = env_int("RELEASE_CHANNEL_ID")
BUG_CHANNEL_ID = env_int("BUG_CHANNEL_ID")
SUGGEST_CHANNEL_ID = env_int("SUGGEST_CHANNEL_ID")
RELEASE_PING_ROLE_ID = env_int("RELEASE_PING_ROLE_ID")
GITHUB_REPO = os.environ.get("GITHUB_REPO", "patchon-t/pacitum-browser").strip()
GITHUB_TOKEN = os.environ.get("GITHUB_TOKEN", "").strip()
GITHUB_ISSUES = os.environ.get("GITHUB_ISSUES", "bugs").strip().lower()   # bugs | all | none
POLL_MINUTES = max(2, env_int("POLL_MINUTES") or 10)
STATE_DIR = pathlib.Path(os.environ.get("STATE_DIR") or HERE)
STATE_FILE = STATE_DIR / "state.json"

ACCENT = 0x5B6CFF   # Pacitum blue

STATUSES = {
    "bug": [("confirmed", "Confirmed", "📌", 0xF5A524), ("fixed", "Fixed", "✅", 0x2EB67D),
            ("cantrepro", "Can't reproduce", "🔁", 0x8A8F98)],
    "idea": [("planned", "Planned", "🗓️", 0x5B6CFF), ("done", "Done", "✅", 0x2EB67D),
             ("declined", "Declined", "✖️", 0x8A8F98)],
}


# ---------------------------------------------------------------- state

def read_state() -> dict:
    try:
        return json.loads(STATE_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def write_state(state: dict) -> None:
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    tmp = STATE_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(state, indent=2), encoding="utf-8")
    tmp.replace(STATE_FILE)


# ---------------------------------------------------------------- GitHub

class GitHub:
    def __init__(self) -> None:
        self.session: aiohttp.ClientSession | None = None

    async def start(self) -> None:
        headers = {"Accept": "application/vnd.github+json", "User-Agent": "pacitum-discord-bot"}
        if GITHUB_TOKEN:
            headers["Authorization"] = f"Bearer {GITHUB_TOKEN}"
        self.session = aiohttp.ClientSession(headers=headers, timeout=aiohttp.ClientTimeout(total=30))

    async def close(self) -> None:
        if self.session:
            await self.session.close()

    async def release(self, version: str | None = None) -> dict | None:
        """The latest release, or the one for `version` ("2.0.5" or "v2.0.5")."""
        assert self.session
        if version:
            tag = version if version.lower().startswith("v") else f"v{version}"
            url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/tags/{tag}"
        else:
            url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
        async with self.session.get(url) as r:
            if r.status == 404:
                return None
            r.raise_for_status()
            return await r.json()

    async def create_issue(self, title: str, body: str, labels: list[str]) -> str | None:
        """Opens an issue and returns its URL (None without a token or on failure)."""
        if not GITHUB_TOKEN or not self.session:
            return None
        url = f"https://api.github.com/repos/{GITHUB_REPO}/issues"
        for attempt_labels in (labels, []):   # retry without labels if they can't be set
            async with self.session.post(url, json={"title": title, "body": body, "labels": attempt_labels}) as r:
                if r.status in (200, 201):
                    return (await r.json()).get("html_url")
                log.warning("GitHub issue failed (%s): %s", r.status, (await r.text())[:300])
                if r.status != 422:
                    return None
        return None


github = GitHub()


# ---------------------------------------------------------------- release notes

def split_message(text: str, limit: int = 1900) -> list[str]:
    """Splits Markdown into Discord-sized messages, at section and line breaks."""
    chunks, current = [], ""
    for line in text.splitlines():
        while len(line) > limit:   # a single very long line
            if current:
                chunks.append(current)
                current = ""
            chunks.append(line[:limit])
            line = line[limit:]
        starts_section = line.startswith("## ") and len(current) > limit // 2
        if len(current) + len(line) + 1 > limit or starts_section:
            chunks.append(current)
            current = ""
        current += line + "\n"
    if current.strip():
        chunks.append(current)
    return [c.strip("\n") for c in chunks if c.strip()]


ASSET_LABELS = [  # (pattern, button label), first match wins
    (r"\.exe$", "Windows"),
    (r"\.deb$", "Linux .deb"),
    (r"\.rpm$", "Linux .rpm"),
    (r"\.appimage$", "AppImage"),
    (r"\.zip$", "Windows .zip"),
    (r"\.tar\.(gz|xz)$", "Linux .tar"),
]


def download_view(release: dict) -> discord.ui.View:
    found: dict[int, tuple[str, str]] = {}   # ASSET_LABELS index -> (label, url), so Windows comes first
    for asset in release.get("assets", []):
        name = asset.get("name", "")
        for i, (pattern, label) in enumerate(ASSET_LABELS):
            if re.search(pattern, name, re.IGNORECASE):
                found.setdefault(i, (label, asset["browser_download_url"]))
                break
    view = discord.ui.View(timeout=None)
    for i in sorted(found):
        label, url = found[i]
        view.add_item(discord.ui.Button(label=label, emoji="⬇️", url=url))
    view.add_item(discord.ui.Button(label="Release page", emoji="🔗", url=release["html_url"]))
    return view


def release_messages(release: dict) -> list[str]:
    title = release.get("name") or release.get("tag_name", "")
    body = (release.get("body") or "").replace("\r\n", "\n").strip() or "_No release notes._"
    # The notes start with "# Pacitum Browser x.y.z": use the release title as the heading instead.
    body = re.sub(r"\A#\s+[^\n]*\n+", "", body)
    return split_message(f"# 🚀 {title}\n{body}")


async def send_release(channel: discord.abc.Messageable, release: dict, ping: bool) -> None:
    messages = release_messages(release)
    mention = f"<@&{RELEASE_PING_ROLE_ID}>\n" if ping and RELEASE_PING_ROLE_ID else ""
    allowed = discord.AllowedMentions(roles=True, users=False, everyone=False)
    for i, text in enumerate(messages):
        extra = {"view": download_view(release)} if i == len(messages) - 1 else {}
        await channel.send((mention if i == 0 else "") + text, allowed_mentions=allowed, suppress_embeds=True, **extra)


# ---------------------------------------------------------------- reports and suggestions

def status_view(kind: str) -> discord.ui.View:
    view = discord.ui.View(timeout=None)
    for key, label, emoji, _ in STATUSES[kind]:
        view.add_item(discord.ui.Button(label=label, emoji=emoji, style=discord.ButtonStyle.secondary,
                                        custom_id=f"pacitum:{kind}:{key}"))
    return view


async def post_to_channel(interaction: discord.Interaction, channel_id: int | None, embed: discord.Embed,
                          kind: str, thread_name: str, reactions: tuple[str, ...] = ()) -> discord.Message | None:
    channel = interaction.client.get_channel(channel_id) if channel_id else None
    if not isinstance(channel, discord.TextChannel):
        await interaction.response.send_message("This isn't set up yet: ask an admin to set the channel for it.", ephemeral=True)
        return None
    await interaction.response.defer(ephemeral=True, thinking=True)
    message = await channel.send(embed=embed, view=status_view(kind))
    for emoji in reactions:
        await message.add_reaction(emoji)
    try:
        thread = await message.create_thread(name=thread_name[:95], auto_archive_duration=10080)
        await thread.add_user(interaction.user)
    except discord.HTTPException as e:
        log.warning("thread: %s", e)
    return message


class BugForm(discord.ui.Modal, title="Report a bug in Pacitum"):
    summary = discord.ui.TextInput(label="Short title", placeholder="The video goes black after switching tabs", max_length=100)
    what = discord.ui.TextInput(label="What happened?", style=discord.TextStyle.paragraph,
                                placeholder="What you saw, and what you expected instead", max_length=1500)
    steps = discord.ui.TextInput(label="Steps to make it happen again", style=discord.TextStyle.paragraph, required=False,
                                 placeholder="1. Open YouTube\n2. Switch to another tab\n3. Come back", max_length=1000)
    version = discord.ui.TextInput(label="Pacitum version", placeholder="2.0.5 (Help › About Pacitum)", required=False, max_length=30)
    system = discord.ui.TextInput(label="System", placeholder="Windows 11 / Ubuntu 24.04", required=False, max_length=60)

    async def on_submit(self, interaction: discord.Interaction) -> None:
        embed = discord.Embed(title=f"🐞 {self.summary.value}", description=self.what.value, color=0xE5484D)
        if self.steps.value:
            embed.add_field(name="Steps", value=self.steps.value[:1024], inline=False)
        embed.add_field(name="Version", value=self.version.value or "—")
        embed.add_field(name="System", value=self.system.value or "—")
        embed.add_field(name="Status", value="🆕 New")
        embed.set_author(name=interaction.user.display_name, icon_url=interaction.user.display_avatar.url)
        message = await post_to_channel(interaction, BUG_CHANNEL_ID, embed, "bug", f"🐞 {self.summary.value}")
        if not message:
            return
        issue = None
        if GITHUB_ISSUES in ("bugs", "all"):
            body = (f"**Reported on Discord by {interaction.user.display_name}** ({message.jump_url})\n\n"
                    f"### What happened\n{self.what.value}\n\n"
                    + (f"### Steps\n{self.steps.value}\n\n" if self.steps.value else "")
                    + f"**Version:** {self.version.value or '—'}  \n**System:** {self.system.value or '—'}")
            issue = await github.create_issue(self.summary.value, body, ["bug", "discord"])
            if issue:
                embed.add_field(name="GitHub", value=f"[Issue]({issue})")
                await message.edit(embed=embed)
        await interaction.followup.send(f"Thanks! Your report is in {message.jump_url}. "
                                        "Follow the thread there in case the developers have a question.", ephemeral=True)


class SuggestForm(discord.ui.Modal, title="Suggest an idea for Pacitum"):
    summary = discord.ui.TextInput(label="Your idea in one line", placeholder="Vertical tabs", max_length=100)
    details = discord.ui.TextInput(label="Describe it", style=discord.TextStyle.paragraph,
                                   placeholder="How it would work", max_length=1500)
    why = discord.ui.TextInput(label="Why would it help?", style=discord.TextStyle.paragraph, required=False, max_length=800)

    async def on_submit(self, interaction: discord.Interaction) -> None:
        embed = discord.Embed(title=f"💡 {self.summary.value}", description=self.details.value, color=0xF5A524)
        if self.why.value:
            embed.add_field(name="Why", value=self.why.value[:1024], inline=False)
        embed.add_field(name="Status", value="🆕 New")
        embed.set_author(name=interaction.user.display_name, icon_url=interaction.user.display_avatar.url)
        embed.set_footer(text="Vote with 👍 / 👎")
        message = await post_to_channel(interaction, SUGGEST_CHANNEL_ID, embed, "idea", f"💡 {self.summary.value}", ("👍", "👎"))
        if not message:
            return
        if GITHUB_ISSUES == "all":
            issue = await github.create_issue(self.summary.value,
                                              f"**Suggested on Discord by {interaction.user.display_name}** ({message.jump_url})\n\n"
                                              f"{self.details.value}\n\n" + (f"### Why\n{self.why.value}" if self.why.value else ""),
                                              ["enhancement", "discord"])
            if issue:
                embed.add_field(name="GitHub", value=f"[Issue]({issue})")
                await message.edit(embed=embed)
        await interaction.followup.send(f"Thanks for the idea! It's posted at {message.jump_url}.", ephemeral=True)


# ---------------------------------------------------------------- the bot

class PacitumBot(discord.Client):
    def __init__(self) -> None:
        super().__init__(intents=discord.Intents.default())
        self.tree = app_commands.CommandTree(self)

    async def setup_hook(self) -> None:
        await github.start()
        # Status buttons are handled in on_interaction by their custom_id, so they keep working after a restart.
        if GUILD_ID:   # commands show up at once in this server
            guild = discord.Object(id=GUILD_ID)
            self.tree.copy_global_to(guild=guild)
            await self.tree.sync(guild=guild)
        else:
            await self.tree.sync()
        self.check_releases.start()

    async def close(self) -> None:
        await github.close()
        await super().close()

    async def on_ready(self) -> None:
        log.info("Signed in as %s", self.user)
        await self.change_presence(activity=discord.Activity(type=discord.ActivityType.watching, name="for /bug and /suggest"))

    async def on_interaction(self, interaction: discord.Interaction) -> None:
        cid = (interaction.data or {}).get("custom_id", "") if interaction.type == discord.InteractionType.component else ""
        if not cid.startswith("pacitum:"):
            return
        _, kind, key = cid.split(":", 2)
        perms = getattr(interaction.user, "guild_permissions", None)
        if not perms or not perms.manage_messages:
            await interaction.response.send_message("Only the Pacitum team can change the status.", ephemeral=True)
            return
        status = next((s for s in STATUSES.get(kind, []) if s[0] == key), None)
        if not status or not interaction.message or not interaction.message.embeds:
            return
        _, label, emoji, color = status
        embed = interaction.message.embeds[0]
        embed.color = color
        for i, field in enumerate(embed.fields):
            if field.name == "Status":
                embed.set_field_at(i, name="Status", value=f"{emoji} {label} · by {interaction.user.display_name}", inline=field.inline)
        await interaction.response.edit_message(embed=embed)
        thread = interaction.message.thread
        if thread:
            try:
                await thread.send(f"{emoji} Status changed to **{label}** by {interaction.user.mention}.",
                                  allowed_mentions=discord.AllowedMentions.none())
            except discord.HTTPException:
                pass

    @tasks.loop(minutes=POLL_MINUTES)
    async def check_releases(self) -> None:
        if not RELEASE_CHANNEL_ID:
            return
        try:
            release = await github.release()
        except Exception as e:   # network trouble: try again next time
            log.warning("release check failed: %s", e)
            return
        if not release:
            return
        state = read_state()
        tag = release.get("tag_name")
        if "last_release" not in state:   # first start: don't repost the current release
            state["last_release"] = tag
            write_state(state)
            log.info("Watching releases from %s", tag)
            return
        if tag == state["last_release"]:
            return
        channel = self.get_channel(RELEASE_CHANNEL_ID)
        if channel is None:
            log.warning("release channel %s not found", RELEASE_CHANNEL_ID)
            return
        await send_release(channel, release, ping=True)
        state["last_release"] = tag
        write_state(state)
        log.info("Posted release %s", tag)

    @check_releases.before_loop
    async def before_check(self) -> None:
        await self.wait_until_ready()


bot = PacitumBot()


@bot.tree.command(name="bug", description="Report a bug in Pacitum Browser")
async def bug_command(interaction: discord.Interaction) -> None:
    await interaction.response.send_modal(BugForm())


@bot.tree.command(name="suggest", description="Suggest a new feature or idea for Pacitum Browser")
async def suggest_command(interaction: discord.Interaction) -> None:
    await interaction.response.send_modal(SuggestForm())


@bot.tree.command(name="release", description="Show the release notes of Pacitum Browser")
@app_commands.describe(version="A version such as 2.0.5 (default: the latest)")
async def release_command(interaction: discord.Interaction, version: str | None = None) -> None:
    await interaction.response.defer(thinking=True)
    try:
        release = await github.release(version)
    except Exception as e:
        await interaction.followup.send(f"Couldn't reach GitHub right now ({e.__class__.__name__}). Try again in a minute.")
        return
    if not release:
        await interaction.followup.send(f"There's no release {version}." if version else "There's no release yet.")
        return
    messages = release_messages(release)
    for i, text in enumerate(messages):
        extra = {"view": download_view(release)} if i == len(messages) - 1 else {}
        await interaction.followup.send(text, suppress_embeds=True, **extra)


@bot.tree.command(name="download", description="Get the latest Pacitum Browser")
async def download_command(interaction: discord.Interaction) -> None:
    await interaction.response.defer(thinking=True)
    try:
        release = await github.release()
    except Exception:
        release = None
    if not release:
        await interaction.followup.send(f"Download Pacitum from https://github.com/{GITHUB_REPO}/releases")
        return
    await interaction.followup.send(f"⬇️ **{release.get('name') or release['tag_name']}** is the latest version.", view=download_view(release))


@bot.tree.command(name="post-release", description="(Team) Post the latest release notes to the release-note channel")
@app_commands.default_permissions(manage_guild=True)
@app_commands.describe(version="A version such as 2.0.5 (default: the latest)", ping="Mention the release role")
async def post_release_command(interaction: discord.Interaction, version: str | None = None, ping: bool = False) -> None:
    channel = bot.get_channel(RELEASE_CHANNEL_ID) if RELEASE_CHANNEL_ID else None
    if channel is None:
        await interaction.response.send_message("RELEASE_CHANNEL_ID isn't set.", ephemeral=True)
        return
    await interaction.response.defer(ephemeral=True, thinking=True)
    release = await github.release(version)
    if not release:
        await interaction.followup.send("No such release.", ephemeral=True)
        return
    await send_release(channel, release, ping=ping)
    if not version:
        state = read_state()
        state["last_release"] = release.get("tag_name")
        write_state(state)
    await interaction.followup.send(f"Posted {release.get('tag_name')} in {channel.mention}.", ephemeral=True)


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    if not TOKEN:
        raise SystemExit("DISCORD_TOKEN is missing: put it in discord-bot/.env (see README.md).")
    bot.run(TOKEN, log_handler=None)


if __name__ == "__main__":
    main()
