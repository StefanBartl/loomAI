# Anschluss-Punkt: documentation.nvim-Checklisten als Agent-Task

Gegenstück zu `E:\repos\docmap-desktop\docs\AGENT_CHECKLIST_RUNNER.md`. Dort
steht die Entscheidung und die Aufwandsschätzung aus Sicht von
`documentation.nvim`/`docmap-desktop`; hier steht, was das für loomAI
bedeutet und was hier davon schon existiert.

## Worum es geht

`documentation.nvim` hat eine Checklist-Funktion: handverifizierte Fakten,
jeweils an eine `@ref path:line` geheftet, mit einem `@verified YYYY-MM-DD`.
Ein Mensch schreibt heute das Verdikt; das Tool prüft nur, ob die zitierte
Zeile sich seither geändert hat (`documentation.nvim/docs/checklist_format.md`).

Die offene Idee (dort als **L6** geführt, "Have an agent run the
checklists") ist, dieses Verdikt von einem Modell vorschlagen zu lassen,
statt es nur zu verfolgen. Zwei Bedingungen sind dabei nicht verhandelbar,
weil sie aus der Grundhaltung des ganzen `documentation.nvim`-Ökosystems
kommen (*"ein selbstsicherer falscher Befund ist schlimmer als ein
unvollständiger"*):

1. Ein maschinell erzeugtes Verdikt darf niemals aussehen wie ein von Hand
   geprüfter Fakt (`@verified` bleibt reserviert für einen Menschen).
2. Eine Agenten-Änderung ist ein **Vorschlag**, kein Ergebnis — es darf
   keinen Weg geben, bei dem derselbe Akteur vorschlägt und verifiziert.

## Warum das hierher gehört, nicht zu `ai.nvim`

`ai.nvim`s eigenes `docs/scope.md` zieht die Grenze selbst: es deckt nur
Single-Turn Frage/Antwort ab und verweist alles, was nach mehrstufigem,
autonomem Agenten oder Human-in-the-Loop-Entscheidung aussieht, ausdrücklich
an ein *separates* Projekt — dort nur "das Agenten-Framework-Projekt"
genannt, aber es ist loomAI (`ai.nvim`s `loomai`-Provider spricht schon
heute mit `/ask`/`/ask/stream` hier).

Bedingung 2 oben ist praktisch wortgleich das, was Abschnitt 8 dieses
Architekturplans (Human-in-the-Loop / Decision Control) beschreibt: ein
**Soft Checkpoint** meldet die KI-Wahl plus Rollback-Möglichkeit, statt sie
einfach durchzusetzen. Das noch einmal in `documentation.nvim` nachzubauen,
würde ein Konzept duplizieren, das hier bereits eingeplant ist.

## Ist-Stand hier, geprüft am Code (2026-09-15)

| Baustein | Zustand |
|---|---|
| `POST /ask`, `POST /ask/stream` | **Fertig.** Vier Backends (Ollama/OpenAI/Anthropic/Gemini) per Modellname-Präfix. Für den einfachen Fall — ein Prompt pro Checklist-Item, ein Text zurück — reicht das bereits, genau wie über `ai.nvim`s Provider-Schicht. |
| `POST /decision` | **Existiert als Route, nicht als Queue.** `main.cpp` (Zeile ~232) protokolliert die menschliche Antwort nur als Dashboard-Event und antwortet sofort mit `{"ok":true}` — kein Pausieren/Fortsetzen irgendeines Agenten, weil dort noch nichts läuft, das pausiert werden könnte. Setzt Bedingung 2 oben also noch **nicht** um. |
| Orchestrator / DecisionQueue / Agent-Worker | **Nicht gebaut.** Phase 3 der eigenen Roadmap (`ki-agenten-framework-architektur.md`, Abschnitt 11), jede Box offen. |
| Dashboard-Decision-UI (Modal Hard-Stop, Rollback-Liste) | **Nicht gebaut.** Phase 4, jede Box offen. |
| Sandbox (Podman, Snapshots) | **Nicht gebaut** — für diesen Anwendungsfall auch nicht nötig: ein Checklist-Verdikt ist Text, keine Container-Operation. |

Ein Aufruf für ein Checklist-Item sieht heute also **exakt** so aus wie
jeder andere `/ask`-Aufruf über `ai.nvim`s `loomai`-Provider. Der Teil, der
loomAI zum *architektonisch* richtigen Anschlusspunkt macht — die
DecisionQueue, die "Vorschlag, kein Ergebnis" selbst durchsetzt, statt dass
`documentation.nvim` das nachbauen muss — existiert im Code noch nicht.

## Zwei Ausbaustufen

**A. Nur gegen `/ask`.** `documentation.nvim` ruft `/ask` direkt pro
Checklist-Item auf und setzt "Vorschlag, kein Ergebnis" selbst durch (neuer
Tag getrennt von `@verified`, Report-first-Oberfläche, eigener Writer für
"gewählte Items anwenden"). **Aufwand hier: keiner** — bestehende Endpoints
reichen unverändert.

**B. Über die DecisionQueue.** Jedes Checklist-Item geht als Soft Decision
durch den Orchestrator; loomAI selbst trägt Vorschlag/Rollback-Semantik,
`documentation.nvim` zeigt nur noch an, was über `/events` zurückkommt.
Architektonisch der sauberere Anschluss, aber abhängig von Phase 3 und 4 —
die noch nicht begonnen sind.

## Aufwandsabschätzung (loomAI-Seite)

| Variante | Aufwand | Anmerkung |
|---|---|---|
| A (nur `/ask`) | **0** | Nichts zu bauen — der bestehende Endpoint ist bereits genau für diese Form (ein Prompt, eine Antwort) gedacht. |
| B, minimal (`/decision` von Log-Event zu echter Queue mit Pause/Resume + einfachster Soft-Checkpoint-Fluss, ohne Sandbox/Agent-Worker-Threads) | **~1–2 Sessions für sich genommen**, aber real eher **M–L**, weil es den Rest von Phase 3 mitzieht (Orchestrator-Grundgerüst, EventBus-Anbindung an eine echte Queue statt an reines Event-Pushing) — nicht isolierbar als reine "für documentation.nvim"-Erweiterung. | Kein neuer Aufwand speziell wegen `documentation.nvim`; das ist ohnehin der erste reale Schritt in Phase 3. |
| B, vollständig (Dashboard-Decision-UI mit Rollback-Liste, Snapshot-Anbindung) | **nicht hier sinnvoll geschätzt** | Gehört zur eigenen Roadmap, Phase 3+4, unabhängig von diesem Anwendungsfall. |

**Empfehlung:** Variante A kostet hier nichts und blockiert nichts — sie
kann sofort genutzt werden, sobald `documentation.nvim` seine Seite baut
(siehe die dortige Datei für deren Aufwand: ~2,5–3 Sessions). Variante B
nicht vorziehen, nur weil dieser eine Anwendungsfall danach fragt — Phase 3
so anzugehen, wie es die eigene Roadmap schon vorsieht, trägt diesen
Anwendungsfall automatisch mit, wenn es an der Zeit ist.

## Erweiterung: Rules-Worklist (L10)

Derselbe Anschluss trägt noch einen zweiten Eingang. Neben den
Checklist-Items (L6) sollen die **manuellen Regeln** aus `rules.nvim` — die
ohne automatischen `check` — an einen Agenten gehen können, angestoßen aus
`docmap-desktop` und aus Neovim. Das Konzept dazu steht in
`E:\repos\docmap-desktop\docs\RULES_AGENT_CONCEPT.md`; hier nur, was es für
loomAI bedeutet.

| Frage | Antwort |
|---|---|
| Was muss loomAI dafür bauen? | **Nichts** für die erste Version. Es bleibt ein `/ask` pro Regel (Prompt + Regeltext + Dateiauszüge, ein Text zurück). Prompt-Bau und Prüfung der Antwort passieren beim Aufrufer, nicht hier. |
| Warum kein Browser-Aufruf aus der App? | Die Origin-Sperre (`set_pre_routing_handler`, 403 bei fremdem `Origin`) lässt Nicht-Browser-Clients durch, blockt aber jede Webview. Die App ruft `/ask` deshalb aus dem Rust-Teil auf, nicht aus dem Frontend. Die Sperre bleibt so, wie sie ist. |
| Was sieht der Nutzer vor dem Senden? | Das aufgelöste Backend. Ob ein Aufruf lokal bleibt oder Dateiauszüge an einen Cloud-Anbieter gehen, entscheidet allein der Modellname-Präfix. `/health` meldet schon heute, welche Cloud-Backends konfiguriert sind (nur Präsenz, nie Schlüssel) — die App zeigt das vor jedem Lauf an. |
| Gibt es optionale Wünsche? | Drei kleine, keiner blockiert: (1) ein optionales `temperature`-Feld auf `/ask`, an alle vier Backends durchgereicht (im Code kommt es bisher nirgends vor) — gleiche Regel, gleiche Dateien, weniger schwankende Antwort; (2) ein `GET /models`, weil die App den Provider im Dialog wählen lässt und loomAI heute keine Modellliste kennt (Routen: `/events`, `/decision`, `/health`, `/ask`, `/ask/stream`); (3) ein `usage`-Feld im letzten Event von `/ask/stream`, das bisher nur `delta`, `error` und `[DONE]` sendet — ohne das kann die App die Token im Chat nur schätzen. Kein JSON-Modus: den bieten nicht alle vier Backends in derselben Form an, der Aufrufer verlangt JSON im Prompt und erzwingt es selbst. Zusammen **~1 Session**. |
| DecisionQueue? | Wie oben: nicht vorziehen. Regeln, die einen Agenten brauchen, der selbst im Repo *navigiert*, brauchen Werkzeuge und damit Phase 3/4 — die Rules-Worklist markiert sie als "zu breit für einen einzelnen Aufruf", statt zu raten. |

Dazu kommt ein Chat im Run-Fenster der App. Er bleibt auf `/ask/stream`: der
Aufrufer hält das Transkript und schickt es je Runde mit, weil `/ask` nur ein
`prompt` und ein `system` kennt, keine Nachrichtenliste. Ein Chat, in dem der
Agent das Repo selbst erkundet, braucht Werkzeuge und damit Phase 3/4 — er
wird hier nicht vorgezogen.
