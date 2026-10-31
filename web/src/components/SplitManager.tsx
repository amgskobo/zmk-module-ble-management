import { useConnectionKey, useRequestLifetime } from "../requestLifetime";
import { errorKind } from "../logPrivacy";
/**
 * SplitManager Component
 *
 * Manages split keyboard connections (for central and peripheral halves).
 */

import { useContext, useState, useEffect, useCallback } from "react";
import {
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import { SUBSYSTEM_IDENTIFIER } from "../App";
import {
  Request,
  Response,
  SplitInfo,
} from "../proto/zmk/ble_management/ble_management";
import "./SplitManager.css";

export function SplitManager() {
  const app = useContext(ZMKAppContext);
  const index = app?.findSubsystem(SUBSYSTEM_IDENTIFIER)?.index;
  const key = useConnectionKey(app?.state.connection, index);
  return <SplitManagerState key={key} />;
}

function SplitManagerState() {
  const zmkApp = useContext(ZMKAppContext);
  const beginRequest = useRequestLifetime();
  const [splitInfo, setSplitInfo] = useState<SplitInfo | null>(null);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [hasLoadFailed, setHasLoadFailed] = useState(false);

  const subsystem = zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER);

  // Load split info on mount
  const loadSplitInfo = useCallback(
    async () => {
      if (!zmkApp?.state.connection || !subsystem) return;

      const active = beginRequest();
      if (!active()) return;
      setIsLoading(true);
      setError(null);

      try {
        const service = new ZMKCustomSubsystem(
          zmkApp.state.connection,
          subsystem.index
        );

        const request = Request.create({
          getSplitInfo: {},
        });

        const payload = Request.encode(request).finish();
        const responsePayload = await service.callRPC(payload);
        if (!active()) return;
        if (!responsePayload) {
          throw new Error("No split information received");
        }

        const resp = Response.decode(responsePayload);
        if (resp.getSplitInfo?.info) {
          setSplitInfo(resp.getSplitInfo.info);
          setHasLoadFailed(false);
        } else if (resp.error) {
          setError(resp.error.message);
          setHasLoadFailed(true);
        } else {
          throw new Error("Unexpected split information response");
        }
      } catch (err) {
        if (!active()) return;
        console.error("Failed to load split info:", errorKind(err));
        setError(
          `Failed to load split info: ${err instanceof Error ? err.message : "Unknown error"}`
        );
        setHasLoadFailed(true);
      } finally {
        if (active()) setIsLoading(false);
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [zmkApp?.state.connection, subsystem?.index, beginRequest]
  );

  useEffect(() => {
    if (subsystem && zmkApp?.state.connection) {
      loadSplitInfo();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [subsystem?.index, zmkApp?.state.connection, loadSplitInfo]);

  const forgetSplitBond = async () => {
    if (!zmkApp?.state.connection || !subsystem) return;
    if (
      !confirm(
        "Clear the pairing of every host profile? Each host must pair again. The bond between the keyboard halves is kept."
      )
    )
      return;

    const active = beginRequest();
    if (!active()) return;
    setIsLoading(true);
    setError(null);

    try {
      const service = new ZMKCustomSubsystem(
        zmkApp.state.connection,
        subsystem.index
      );

      const request = Request.create({
        forgetSplitBond: {},
      });

      const payload = Request.encode(request).finish();
      const responsePayload = await service.callRPC(payload);
      if (!active()) return;

      if (responsePayload) {
        const resp = Response.decode(responsePayload);
        if (resp.forgetSplitBond?.success) {
          alert(
            "Host pairings cleared and profile 0 selected. Pair each host again."
          );
          await loadSplitInfo();
        } else if (resp.error) {
          setError(resp.error.message);
        } else {
          setError("Failed to clear host pairings");
        }
      }
    } catch (err) {
      if (!active()) return;
      console.error("Failed to clear host pairings:", errorKind(err));
      setError(
        `Failed to clear host pairings: ${err instanceof Error ? err.message : "Unknown error"}`
      );
    } finally {
      if (active()) setIsLoading(false);
    }
  };

  if (!subsystem) {
    return null;
  }

  // Keep the initial unknown state and confirmed non-split devices hidden.
  // A failed read needs a recovery view that remains visible during its retry.
  if (splitInfo?.isSplit === false || (!splitInfo && !hasLoadFailed)) {
    return null;
  }

  return (
    <section className="card split-manager">
      <h2>⌨️ Split Keyboard</h2>

      {error && (
        <div className="error-message">
          <p>🚨 {error}</p>
        </div>
      )}

      {isLoading && !splitInfo && <p>⏳ Loading split information...</p>}

      {splitInfo && (
        <div className="split-info">
          <div className="info-grid">
            <div className="info-item">
              <strong>Role:</strong>{" "}
              {splitInfo.isCentral ? "Central" : "Peripheral"}
            </div>

            {splitInfo.isCentral && (
              <div className="info-item">
                <strong>Peripheral Status:</strong>{" "}
                {splitInfo.peripheralConnected ? (
                  <span className="status-connected">✓ Connected</span>
                ) : (
                  <span className="status-disconnected">✗ Disconnected</span>
                )}
              </div>
            )}

            {!splitInfo.isCentral && (
              <div className="info-item">
                <strong>Central Bonded:</strong>{" "}
                {splitInfo.centralBonded ? (
                  <span className="status-bonded">✓ Bonded</span>
                ) : (
                  <span className="status-not-bonded">✗ Not Bonded</span>
                )}
              </div>
            )}
          </div>

          <div className="split-actions">
            <p className="warning-text">
              ⚠️ This clears the pairing of every host profile and selects
              profile 0. It does not reset the bond between the keyboard halves;
              to re-pair the halves, flash settings_reset to both of them.
            </p>
            <button
              className="btn btn-danger"
              onClick={forgetSplitBond}
              disabled={isLoading}
            >
              🔄 Clear All Host Pairings
            </button>
          </div>
        </div>
      )}

      <button
        className="btn btn-secondary"
        onClick={loadSplitInfo}
        disabled={isLoading}
      >
        🔄 Refresh
      </button>
    </section>
  );
}
