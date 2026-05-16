# enucleation
\# MonoView \- Meta Quest single\-lens display controller

\> **Note:** This project is experimental, unofficial, and not affiliated with or endorsed by Meta.

---

\## Overview

**MonoView** is a concept Meta Quest application intended for users who only need **one eye** active in VR or MR scenarios, such as **enucleated users** or people who strongly prefer single\-eye operation.

The goal is to provide:

\- **Per\-eye display control:** Turn **left** or **right** lens output off where technically possible.  
\- **Potential power savings:** Reduce power usage by disabling one display, depending on hardware and platform limitations.  
\- **Comfort options:** Reduce visual overload or discomfort for users who only meaningfully see from one eye.

> Actual power savings and behavior depend on Meta Quest firmware, drivers, and platform permissions. This app can only do what the underlying system allows.

---

\## Features \(\*intended design\*\)

\- **Eye selection UI:**  
  \- **Left eye only** mode  
  \- **Right eye only** mode  
  \- **Both eyes** \(default / fallback\)

\- **Runtime switching:**  
  \- Change active eye mode from an in\-app menu.  
  \- Optional quick\-toggle actions \(e\.g\. controller button combo or hand gesture, if permitted\).

\- **Status indicators:**  
  \- Clear on\-screen indicator of which eye is currently active.  
  \- Warning banner when running in single\-eye mode.

---

\## Technical notes

\- **Platform:** Meta Quest \(Android / OpenXR\)  
\- **Engine:** Unity or Unreal \(implementation\-dependent\)  
\- **Rendering approach \(conceptual\):**  
  \- Use per\-eye render passes and selectively skip or blank one eye.  
  \- Where true panel power\-down is not possible, render a solid black frame or minimal scene to the unused eye.  
  \- Respect all platform constraints and store policies.

> Important: Some devices may **not** allow true hardware power\-off for a single panel. In those cases, MonoView can only simulate single\-eye mode visually.

---

\## Accessibility goals

\- **Support enucleated users:**  
  \- Avoid wasting GPU and rendering effort on an eye that is not used.  
  \- Reduce distracting artifacts in the non\-seeing eye.

\- **Comfort customization:**  
  \- Let users experiment with which eye feels best as the primary view.  
  \- Provide large, high\-contrast UI for mode switching.

---

\## Safety and medical disclaimer

\- **Not a medical device:**  
  \- MonoView does **not** diagnose, treat, or cure any condition.  
  \- It is purely a visual and comfort customization tool.

\- **Consult professionals:**  
  \- If you have had eye surgery, trauma, or other medical conditions, consult a medical professional before extended VR use.  
  \- Follow all Meta health and safety guidelines for VR usage.

---

\## Usage \(\*intended flow\*\)

1\. **Launch the app** on your Meta Quest device.  
2\. From the **main menu**, choose:  
   \- **Left eye only**  
   \- **Right eye only**  
   \- **Both eyes**  
3\. Confirm the selection and verify that the view feels comfortable.  
4\. Use the **in\-app menu** at any time to switch modes or return to both\-eye mode.

---

\## Limitations

\- Actual power savings may be **minimal or none**, depending on:  
  \- Device hardware  
  \- System firmware  
  \- Meta platform restrictions

\- Some apps or system overlays may still render to both eyes regardless of MonoView settings.  
\- Store policies may restrict distribution of apps that alter core display behavior; this project may remain **side\-load only**.

---

\## Project status

This repository is currently a **design and experimentation scaffold**.  
Implementation details will depend on:

\- Meta Quest SDK capabilities  
\- OpenXR extensions for per\-eye control  
\- Store and platform policy review

Contributions, experiments, and research into safe, compliant ways to support single\-eye users are welcome.

---

\## License

This project is intended to be released under a **permissive open\-source license** \(e\.g\. MIT or Apache\-2\.0\).  
See `LICENSE` for details once added.
