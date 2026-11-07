# Pacitum Discord bot

The **Pacitum** bot for the Pacitum Browser Discord server.

| What | How |
|---|---|
| **Release notes** | Every new GitHub release is posted to `#pacitum-release-note` automatically (checked every 10 minutes), with download buttons. `/release` shows the notes of any version, `/download` the latest downloads. |
| **Bug reports** | `/bug` opens a form (title, what happened, steps, version, system). The report goes to `#pacitum-bug-report` with a discussion thread, and, with a GitHub token, a GitHub issue. |
| **Suggestions** | `/suggest` opens a form. The idea goes to `#pacitum-suggestion` with 👍 / 👎 votes and a thread. |
| **Status** | Under each report / idea, the team (anyone with *Manage Messages*) clicks **Confirmed / Fixed / Can't reproduce** or **Planned / Done / Declined**. The post changes colour and the thread is told. |
| **Team only** | `/post-release` posts a release by hand (for example the current one, the first time). |

The bot only needs to be running somewhere (your PC or a small server) to work. It uses no AI and costs nothing.

## 1. Create the bot in Discord (once)

1. Open <https://discord.com/developers/applications> and sign in with your Discord account.
2. **New Application**, name it **Pacitum**, accept, **Create**.
3. **General Information**: set the app icon to `bot-avatar.png` (in this folder) and a description, then **Save Changes**.
4. **Bot** (left menu):
   - Set the bot's icon to `bot-avatar.png` too, and its username to **Pacitum**.
   - **Reset Token**, then **Copy**. This is `DISCORD_TOKEN`. Keep it secret: anyone with it controls the bot. If it leaks, reset it here.
   - Leave all "Privileged Gateway Intents" **off**. The bot doesn't need them.
5. **OAuth2** › **OAuth2 URL Generator**:
   - Scopes: **bot** and **applications.commands**.
   - Bot permissions: **View Channels**, **Send Messages**, **Send Messages in Threads**, **Create Public Threads**, **Embed Links**, **Add Reactions**, **Read Message History**, **Mention Everyone** (only needed to ping a release role).
   - Open the generated URL, pick your server, **Authorize**. Pacitum joins the server.

## 2. Get the IDs

1. In Discord: **User Settings › Advanced › Developer Mode** on.
2. Right-click your **server icon › Copy Server ID** → `GUILD_ID`.
3. Right-click each channel › **Copy Channel ID**:
   - `#pacitum-release-note` → `RELEASE_CHANNEL_ID`
   - `#pacitum-bug-report` → `BUG_CHANNEL_ID`
   - `#pacitum-suggestion` → `SUGGEST_CHANNEL_ID`
4. Optional: make a role such as `@Pacitum Updates`, right-click it › **Copy Role ID** → `RELEASE_PING_ROLE_ID`. Members who take that role are pinged on new releases.

Channel tips:
- `#pacitum-release-note`: deny **Send Messages** for @everyone, allow it for the Pacitum bot, so only release posts appear there.
- `#pacitum-bug-report` / `#pacitum-suggestion`: you can deny **Send Messages** for @everyone (but keep **Send Messages in Threads**). Then reports only come in through the forms and people talk in the threads.

## 3. Settings

Copy `.env.example` to `.env` (in this folder) and fill in the token and the IDs.

Optional, to open a GitHub issue for each bug report:
1. GitHub › **Settings › Developer settings › Personal access tokens › Fine-grained tokens › Generate new token**.
2. Repository access: only `pacitum-browser`. Permissions: **Issues: Read and write**.
3. Put the token in `GITHUB_TOKEN`. `GITHUB_ISSUES=all` also turns suggestions into issues.

## 4. Run it

**Windows**: install Python 3.10 or newer from <https://www.python.org/downloads/> (tick *Add Python to PATH*), then double-click `start.bat`. The first start installs what it needs.

**Linux / a server**: `./start.sh`

**Docker**:
```sh
docker build -t pacitum-bot .
docker run -d --restart unless-stopped --env-file .env -v pacitum-bot:/bot/data --name pacitum-bot pacitum-bot
```

When it says `Signed in as Pacitum#…`, type `/` in your server: `/bug`, `/suggest`, `/release` and `/download` appear.

The bot is only online while it runs. For 24/7, run it on a computer that stays on, or on a small server: a free Oracle Cloud VM, or a few dollars a month on Railway, Fly.io or any VPS.

**First start**: the bot remembers the current release (2.0.5) without posting it, so old releases are not reposted. To post 2.0.5 now, use `/post-release` (team only). From then on, every new release is posted by itself.
