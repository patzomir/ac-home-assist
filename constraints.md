# Product Constraints

## Smart Plug in AC Power Path

### Technical reality
- Nous A7Z / Sonoff S26R2ZB are rated 16A / 3600W
- A typical residential inverter AC (e.g. 18000 BTU / 5kW cooling) draws ~6.8A max
- Modern inverter ACs have soft-start — no high inrush current (unlike fixed-speed compressors)
- 6.8A on a 16A plug = 43% load, well within the 80% continuous derating rule
- Standard Schuko wall sockets are also rated 16A — the smart plug does not add a weaker link in the chain
- Relay wear concern: inductive motor loads cause more wear than resistive — but inverter ACs are not a direct motor load at the plug. The relay switches the inverter's input stage (bridge rectifier + capacitors), which is a capacitive/resistive load at mains level. The inverter then soft-starts the compressor internally — no back-EMF, much lower inrush than a fixed-speed motor.
- Estimated relay life at 2 switches/day: **30–50+ years** for inverter ACs (vs. 10–20 years for fixed-speed). Non-issue in practice.

### Liability and scale
The 16A rating is not the issue — the issue is liability at scale:
- If anything goes wrong (house fire, damaged AC), your hardware is in the chain
- You can't verify customer installation context: worn socket contacts, dodgy wiring, fixed-speed vs. inverter AC
- "Check your nameplate and socket condition" is not a message you can deliver verbally to every customer

**Resolution:** This is addressed through a printed installation guide shipped with the product. The safety condition is simple and binary: if the customer already uses a standard Schuko socket for their AC, the smart plug is a like-for-like replacement — same 16A rating. Anything outside that is explicitly out of scope. This is the same approach used by Shelly, Sonoff, Tapo, and every other smart plug vendor. The installation guide includes a pre-install checklist that documents customer acknowledgment. For property damage claims, this shifts liability to the customer for out-of-spec installations. Bodily injury claims under EU product liability law cannot be fully disclaimed, but the risk profile is identical to the rest of the market.

### Conclusion
Smart plug is technically fine — same 16A rating as the socket it plugs into, well within load limits for inverter ACs. Viable as a shipped product component when accompanied by a clear installation guide with pre-install checklist. Liability exposure is managed through documentation, not avoided by removing the component.

---

## Energy Visibility is Non-Negotiable

The core value proposition is cost visibility — "tonight cost you 0.42 BGN instead of 1.20 BGN." Without this, the product is just a remote control. Scheduling/setback savings exist without the plug, but are invisible to the user and don't change behavior.

---

## Energy Monitoring Alternatives

### Option 1: Power estimation (chosen for MVP) ✅
- Inputs: AC mode + set temperature (known from IR commands) + outdoor temperature (weather API)
- No additional hardware — works with hub + IR emitter already in the system
- Inverter ACs behave predictably — consumption is modelable from nameplate figures
- Answers the user's actual question ("did I save money tonight") without real measurement
- Scales to any customer out of the box

### Option 2: CT clamp sensor (pro/add-on tier)
- **Tuya PJ-MGW1203** (~$22) or **Zemismart SPM01** (~$26)
- Real measurement — but has a critical installation constraint:
  - **CT clamps must be around a single wire (Live only)**
  - Standard AC cables carry L + N together — opposite currents cancel the magnetic field → clamp reads zero
  - Requires access to separated L and N wires (inside socket box, inside AC terminal block, or via a custom junction box)
  - Cannot be done as a simple clip-on install — not viable for mass-market self-install
- Viable for pro installation tier or technically confident users

### Option 3: Smart plug (monitoring only)
- Valid for any installation where a standard 16A Schuko socket is already in use
- Scalable as a product component when shipped with a printed installation guide (see above)

---

## Decision
Power estimation is the MVP approach. No additional hardware, scales to any customer. CT clamp is a pro/add-on tier option for users willing to do proper installation.
