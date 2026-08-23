# Message Relays

A message relay is a masked-ID gateway rule: every received frame whose CAN ID matches the rule is forwarded, whole and unmodified, to the selected target buses. Unlike a receive or transmit message, a relay carries no channels and decodes nothing — the firmware runs its relay rules on **every** received frame, independently of the message table, so a relay forwards traffic the configuration does not otherwise know about.

Use a relay to bridge buses ("everything from the engine bus onto the logger bus"), to pass through a supplier's ID block, or — with Invert Result — to forward everything *except* a matched set. To route one specific message you are already receiving, the section editor's **Gateway Routing (CAN Triple)** group does the same job per message; a relay is the tool for ID ranges and whole-bus forwarding.

## Creating a relay

To add a relay, open **Connections &gt; Communications…**, pick the tab of the bus the frames **arrive on** — a relay listens only on the bus it is defined on — click **New…**, and set **Message Type :** to **Message Relay**. The message framing controls (Alignment, Receive Timeout, CAN FD frame, Message Length, Transmission, channels tab) do not apply to a relay and are disabled or hidden; the **Message Relay** group appears in their place.

<table>
<tr><th>Control</th><th>Meaning</th></tr>
<tr><td><b>Name :</b></td><td>Display name; left empty it defaults to
"Relay 0x<i>address</i>".</td></tr>
<tr><td><b>Address Format :</b></td><td><b>Standard</b> or <b>Extended</b>. A
relay matches only frames of its own extended-ness — a standard-ID rule never
catches extended frames, and vice versa.</td></tr>
<tr><td><b>Base Address :</b></td><td>The ID pattern to match, in hex.</td></tr>
<tr><td><b>Message Bitmask :</b></td><td>The mask, in hex, shown at the same
width as the address (three digits standard, eight extended). A frame matches
when <b>(its ID &amp; mask) == (Base Address &amp; mask)</b>. The label beside
the field shows the value — and reads "= 0  (matches every frame)" for a zero
mask.</td></tr>
<tr><td><b>Invert Result (forward the non-matching frames)</b></td><td>Forwards
the frames that do <i>not</i> match instead.</td></tr>
<tr><td><b>Forward to :</b></td><td>One checkbox per target bus. Only the other
two buses are offered — a relay never forwards back onto its source bus. At
least one target must be ticked; OK refuses otherwise.</td></tr>
</table>

## Mask examples (standard IDs)

<table>
<tr><th>Base Address</th><th>Bitmask</th><th>Forwards</th></tr>
<tr><td>0x000</td><td>0x000</td><td>Every frame on the source bus (mask 0 matches
everything).</td></tr>
<tr><td>0x640</td><td>0x7FF</td><td>Exactly ID 0x640.</td></tr>
<tr><td>0x600</td><td>0x700</td><td>The block 0x600–0x6FF (top three bits must
match).</td></tr>
<tr><td>0x640</td><td>0x7FF, Invert Result</td><td>Everything except
0x640.</td></tr>
</table>

> **Warning:** Bitmask 0 together with Invert Result matches no frame at all — nothing is forwarded. Validation flags this, along with a forward target whose bus Mode is Off (frames relayed to a disabled bus are dropped).

## In the sections list

A relay shows as **Relay** in the Section column, and its Name column shows the forwarding targets ("→ CAN 2, CAN 3", or "(no target)"). The Channels pane reads "(relay — forwards whole frames, no channels)". Because a relay has no channels, retyping an existing receive or transmit message into a relay discards that message's channels and identifiers when the editor is accepted.

## Behaviour and capacity
- Relay rules run on every received frame on their source bus, before and independently of message decoding — a frame can be both decoded by a receive message and forwarded by a relay.
- Forwarded frames keep their ID, length and data unchanged.
- A relay never forwards onto its own source bus, so relay rules cannot loop a frame back where it came from. Two relays on different buses can still forward into each other — take care not to build a two-bus loop.
- The device stores relays in their own table of up to **32 rules**, separate from the 500-entry message table.

> **Note:** Relays are configuration like everything else: they take effect on the device after a Send and persist once saved to flash — see [Online: Send, Get &amp; Flash](online.md). The Config Summary counts them under Device usage ("x/32 relays"); see [Validation &amp; the Config Summary](validation-report.md).

See also: [Communications: Messages &amp; Sections](communications.md) · [Channels](channels.md) · [DBC Import](dbc-import.md)
