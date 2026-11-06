/* Settings: engine + rank table, display, about. */

import { motion } from "motion/react";
import { useApp } from "../state.jsx";
import { importRanks, reveal } from "../lib/engine.js";
import { Button, Panel, ProgressRing, Segmented, softSpring, Switch } from "../components/ui.jsx";
import { PlayingCard } from "../components/Cards.jsx";
import { IconCheck, IconFolder, IconSparkle, IconStop, IconAlert } from "../components/Icons.jsx";

const fmtDur = (s) => (s < 60 ? `${Math.max(1, Math.round(s))} s` : `${Math.round(s / 60)} min`);

export default function Settings() {
  const { settings, setSettings, info, status, ranksLoaded, rankJob, buildRanks, cancelRanks, refreshStatus, toast, presets } = useApp();

  let eta = null;
  if (rankJob && rankJob.done > 0) {
    const el = (performance.now() - rankJob.startedAt) / 1000;
    eta = (el / rankJob.done) * (rankJob.total - rankJob.done);
  }
  const frac = rankJob ? rankJob.done / Math.max(1, rankJob.total) : 0;

  const doImport = async () => {
    const r = await importRanks();
    if (r && r.ok) { toast("Rank table imported", "ok"); setTimeout(refreshStatus, 400); }
    else if (r && r.error) toast(r.error, "error");
  };

  return (
    <div className="settings">
      <Panel title="Preflop rank table">
        <div className="rank-card">
          <motion.div className={`rank-icon ${ranksLoaded ? "ok" : rankJob ? "busy" : "warn"}`} layout transition={softSpring}>
            {rankJob ? <ProgressRing value={frac} size={56} stroke={5} /> : ranksLoaded ? <IconCheck size={26} /> : <IconAlert size={24} />}
            {rankJob && <span className="rank-pct">{Math.floor(frac * 100)}%</span>}
          </motion.div>
          <div className="rank-text">
            {rankJob ? (
              <>
                <h3>Building the rank table…</h3>
                <p>Ranking all 2,598,960 starting hands by equity. {eta != null ? `About ${fmtDur(eta)} left.` : "Estimating time…"} The calculator waits until this finishes.</p>
              </>
            ) : ranksLoaded ? (
              <>
                <h3>Loaded</h3>
                <p>Preflop percentiles and preflop ranges are available.</p>
              </>
            ) : (
              <>
                <h3>Not built yet</h3>
                <p>
                  Preflop percentiles (and ranges with no board) need a one-time table of every starting hand,
                  ranked by equity. Building it takes a few minutes on a modern CPU and is saved for next time.
                </p>
              </>
            )}
            <div className="row-actions">
              {rankJob ? (
                <Button variant="soft" icon={IconStop} onClick={cancelRanks}>Cancel</Button>
              ) : (
                <>
                  <Button variant={ranksLoaded ? "soft" : "primary"} icon={IconSparkle} onClick={buildRanks} disabled={!status}>
                    {ranksLoaded ? "Rebuild" : "Build now"}
                  </Button>
                  <Button icon={IconFolder} onClick={doImport}>Use an existing file…</Button>
                </>
              )}
            </div>
            {info && info.ranks && <code className="path" title={info.ranks} onClick={() => reveal("ranks")}>{info.ranks}</code>}
          </div>
        </div>
      </Panel>

      <Panel title="Display">
        <div className="set-row">
          <div>
            <b>Deck colours</b>
            <p>Four-colour decks make suits easier to tell apart at a glance.</p>
          </div>
          <div className="set-control">
            <div className="deck-preview">
              {[51, 46, 41, 36].map((id) => <PlayingCard key={id} id={id} size="xs" />)}
            </div>
            <Segmented size="sm" value={settings.fourColor} layoutKey="fourcolor" onChange={(v) => setSettings({ fourColor: v })}
              options={[{ value: true, label: "Four colour" }, { value: false, label: "Two colour" }]} />
          </div>
        </div>
        <div className="set-row">
          <div>
            <b>Animations</b>
            <p>Reduced keeps fades but skips motion (also follows your system setting).</p>
          </div>
          <Segmented size="sm" value={settings.motion} layoutKey="motion" onChange={(v) => setSettings({ motion: v })}
            options={[{ value: "full", label: "Full" }, { value: "reduced", label: "Reduced" }]} />
        </div>
        <div className="set-row">
          <div>
            <b>Calculator</b>
            <p>Defaults for new sessions — also on the Run panel.</p>
          </div>
          <div className="set-col">
            <Switch label="Auto-calculate" checked={settings.auto} onChange={(v) => setSettings({ auto: v })} />
            <Switch label="Board percentiles" checked={settings.buildRanks} onChange={(v) => setSettings({ buildRanks: v })} />
          </div>
        </div>
      </Panel>

      <Panel title="Engine">
        <dl className="facts">
          <dt>Status</dt>
          <dd>{status ? <span className="ok-dot">Running</span> : "Starting…"}</dd>
          <dt>Threads</dt><dd>{status ? status.ncpu : "—"}</dd>
          <dt>Presets</dt><dd>{presets.map((p) => `${p.name} ${p.trials.toLocaleString("en-US")}`).join(" · ")}</dd>
          <dt>Config</dt><dd>{info && info.cfg ? <code className="path" onClick={() => reveal("config")}>{info.cfg}</code> : "defaults (run plo5setup to calibrate)"}</dd>
          <dt>Engine</dt><dd>{info ? info.engine : "—"}</dd>
          <dt>App</dt><dd>PLO5 Equity Lab {info ? info.version : ""}</dd>
        </dl>
      </Panel>
    </div>
  );
}
