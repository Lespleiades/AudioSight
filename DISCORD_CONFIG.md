# Discord chat setup

AudioSight can show the latest messages of a Discord text channel in a small window on top of your game, so you can follow the chat without leaving it.

It works with a **Discord bot** that you create yourself. The bot reads **one text channel** of **your** server through Discord's official API, and AudioSight displays the messages. Nothing is injected into the game, and your Discord account is never used.

This takes about 5 minutes and only has to be done once.

---

## Important: 

For sending messages to Discord to work correctly, AudioSight.exe must be running in Administrator mode.

---

## What you need

- A Discord account with the **Manage Server** permission on the server where the chat is (this is always the case on a server you created).
- AudioSight running at least once (so it creates `AudioSight.ini` next to `AudioSight.exe`).

---

## 1. Create the bot

1. Open the [Discord Developer Portal](https://discord.com/developers/applications) and log in.
2. Click **New Application**, give it a name (for example `AudioSight Chat`), accept the terms and click **Create**.
3. In the left menu, in **Overview** section select **Bot**.
4. In the **Bot** page in section **Token** click **Reset Token** (confirm if asked), then **Copy**. This is your **bot token**.
   - Discord shows it **only once**. Paste it somewhere safe right away. If you lose it, just click **Reset Token** again to get a new one.
   - Treat it like a password: whoever has it controls your bot.
5. Still on the **Bot** page, scroll down to **Privileged Gateway Intents** and turn **Message Content Intent** **ON**. Then click **Save Changes**.

> **Do not skip step 5.** Without *Message Content Intent*, Discord sends the messages **empty** and nothing will show up in the overlay.

---

## 2. Invite the bot to your server

1. In the left menu (**Overview** section), open **OAuth2**, then **OAuth2 URL Generator**.
2. Under **Scopes**, tick **`bot`**.
3. Under **Bot Permissions** (it appears after ticking `bot`), tick:
   - **View Channels**
   - **Read Message History**
   - **Send Messages**
4. Copy the **Generated URL** at the bottom of the page and open it in your browser.
5. Choose your server in the list, click **Continue**, then **Authorize**.

The bot now appears in your server's member list (it will be offline: that is normal, AudioSight only reads messages and does not keep the bot online).

---

## 3. Make sure the bot can see the channel

If the channel is visible to everyone, you can skip this step.

If it is a **private channel**:

1. Right-click the channel, then **Edit Channel**, then **Permissions**.
2. Click **Add members or roles** and pick your bot.
3. Allow **View Channel** and **Read Message History**.

---

## 4. Get the channel ID

1. In Discord, open **User Settings**, then left section **Edit Profiles**, and turn **<> Developer** **Developer Mode** **ON** to the both page over **Log Out**.
2. Right-click the text channel you want to display in the channels list and choose **Copy Channel ID**.
   - The text chat of a **voice channel** works too: right-click the voice channel and copy its ID.

The ID is a long number such as `123456789012345678`.

---

## 5. Edit `AudioSight.ini`

`AudioSight.ini` is in the same folder as `AudioSight.exe`. The quickest way to open it: in the AudioSight settings panel (**F7**), click **Edit colors...**, which opens the file in your text editor.

At the end of the file you will find this section (AudioSight adds it on first launch):

```ini
[discord]
bot_token=
channel_id=
```

Fill in both lines, **without quotes and without spaces**:

```ini
[discord]
bot_token=MTIzNDU2Nzg5MDEyMzQ1Njc4.AbCdEf.xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
channel_id=123456789012345678
```

Save the file. **No restart is needed**: AudioSight notices the change and connects by itself.

> If the `[discord]` section is missing, close AudioSight, start it again, and it will be created. You can also type it in yourself, exactly as shown above.

---

## Troubleshooting

The chat window shows a message when something is wrong:

| Message | Cause and fix |
|---|---|
| *Discord chat is not set up yet...* | `bot_token` or `channel_id` is empty. Fill both in the `[discord]` section and save. |
| *bot_token or channel_id looks wrong* | There is a typo or an extra character. The token has only letters, digits, `.`, `-` and `_`. The channel ID has only digits. Copy them again. |
| *the bot token is not valid (401)* | The token was reset or copied wrongly. Click **Reset Token** on the Bot page, copy the new one and paste it again. |
| *the bot is not allowed to read this channel (403)* | The bot lacks **View Channel** or **Read Message History** on that channel. See step 3. |
| *channel not found (404)* | Wrong `channel_id`, or the bot was not invited to the server that contains this channel. See steps 2 and 4. |
| *no connection, retrying...* | No internet, or a firewall is blocking `discord.com`. AudioSight keeps retrying by itself. |
| *waiting for messages...* | Everything works, the channel simply has no messages yet. |

**The window is empty or the messages look blank, with no error:** *Message Content Intent* is off. See step 1, point 5, and remember to click **Save Changes**.

**The chat does not appear at all:** check that **Discord chat** is switched on (F7 panel or tray menu), and that your game runs in *windowed* or *borderless windowed* mode. Like the rest of the overlay, it cannot be drawn over games in exclusive fullscreen mode.

---

## Good to know

- **What is displayed:** the latest messages of the channel, including the ones written by other bots. Text, mentions (`@name`), custom emojis (`:name:`) and bot embeds are shown. Images and files are shown as `[attachment]`.
- **Replying:** the chat window only displays messages. To answer with one key press, use the Quick phrases. For anything else, go back to Discord.
- **One channel at a time:** to follow another channel, change `channel_id`.
- **No direct messages:** a bot can only read channels of servers it has been invited to.
- **Speech to text:** the overlay shows text only. If you want voice conversations transcribed, add a transcription bot to your server (follow that bot's own documentation) and set `channel_id` to the text channel where it writes its transcripts.
- **Update speed:** the channel is checked about every 2.5 seconds.

## Keep your token private

- Never share your `bot_token`, and never post a screenshot or a copy of `AudioSight.ini` that contains it.
- If you think it leaked, go to the Developer Portal, **Bot** page, click **Reset Token**, and paste the new token in the `.ini`. The old one stops working immediately.
- The bot only has permission to **read** one server's channels, but a leaked token should still be replaced.
- If you share AudioSight with other people, each person must create **their own bot** and use their own token. Never include your token in a copy you give away.
