import { act, fireEvent, render, screen, within } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import { SUBSYSTEM_IDENTIFIER } from "../../web/src/App";
import { ProfileManager } from "../../web/src/components/ProfileManager";
import { OutputPriorityManager } from "../../web/src/components/OutputPriorityManager";
import { SplitManager } from "../../web/src/components/SplitManager";
import {
  Request,
  Response,
} from "../../web/src/proto/zmk/ble_management/ble_management";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({ call_rpc: jest.fn() }));

type Operation = Exclude<keyof Response, "error">;
type Shape = Parameters<typeof Response.create>[0];
type Boundary =
  | "matching"
  | "absent"
  | "empty"
  | "unrelated"
  | "mixed error"
  | "mixed unrelated"
  | "blank error"
  | "empty acknowledgement";

const peer = {
  addressType: 0,
  address: new Uint8Array([6, 5, 4, 3, 2, 1]),
};
const replies: Record<Operation, Shape> = {
  getProfiles: {
    getProfiles: {
      maxProfiles: 5,
      profiles: [
        {
          index: 1,
          name: "fixture-host",
          address: "01:02:03:04:05:06",
          peerIdentity: peer,
          isOpen: false,
          isActive: false,
        },
      ],
    },
  },
  getOutputPriority: { getOutputPriority: { priority: 0 } },
  getSplitInfo: { getSplitInfo: { info: { isSplit: true, isCentral: true } } },
  setProfileName: { setProfileName: { success: true } },
  switchProfile: { switchProfile: { success: true } },
  unpairProfile: { unpairProfile: { success: true } },
  setOutputPriority: { setOutputPriority: { success: true } },
  forgetSplitBond: { forgetSplitBond: { success: true } },
};
const reads = ["getProfiles", "getOutputPriority", "getSplitInfo"] as const;
const mutations = [
  "setProfileName",
  "switchProfile",
  "unpairProfile",
  "setOutputPriority",
  "forgetSplitBond",
] as const;
const boundaries: Boundary[] = [
  "matching",
  "absent",
  "empty",
  "unrelated",
  "mixed error",
  "mixed unrelated",
  "blank error",
];

function wire(value: Shape) {
  return {
    custom: {
      call: { payload: Response.encode(Response.create(value)).finish() },
    },
  };
}

function boundaryReply(operation: Operation, boundary: Boundary) {
  const unrelated =
    operation === "getProfiles"
      ? replies.getOutputPriority
      : replies.getProfiles;
  switch (boundary) {
    case "matching":
      return wire(replies[operation]);
    case "absent":
      return {};
    case "empty":
      return wire({});
    case "unrelated":
      return wire(unrelated);
    case "mixed error":
      return wire({
        ...replies[operation],
        error: { message: "Fixture refusal" },
      });
    case "mixed unrelated":
      return wire({ ...replies[operation], ...unrelated });
    case "blank error":
      return wire({ error: { message: "   " } });
    case "empty acknowledgement":
      // The actual firmware uses an explicit bool success, defaulting false.
      return wire({ [operation]: {} });
  }
}

beforeEach(() => {
  jest.clearAllMocks();
  jest.spyOn(console, "error").mockImplementation(() => {});
  jest.spyOn(window, "confirm").mockReturnValue(true);
  jest.spyOn(window, "alert").mockImplementation(() => {});
});
afterEach(() => jest.restoreAllMocks());

async function start(operation: Operation, boundary: Boundary) {
  const current = createConnectedMockZMKApp({
    subsystems: [SUBSYSTEM_IDENTIFIER],
  });
  const cleared = jest.fn();
  let intercept = true;
  const sent: Request[] = [];
  (call_rpc as jest.Mock).mockImplementation((_connection, raw) => {
    const request = Request.decode(raw.custom.call.payload);
    sent.push(request);
    if (request[operation] && intercept)
      return Promise.resolve(boundaryReply(operation, boundary));
    const requestType = Object.keys(replies).find(
      (key) => request[key as Operation],
    ) as Operation;
    if (!requestType) throw new Error("Unexpected fixture request");
    return Promise.resolve(wire(replies[requestType]));
  });
  const panel = operation.toLowerCase().includes("outputpriority") ? (
    <OutputPriorityManager />
  ) : operation === "getSplitInfo" || operation === "forgetSplitBond" ? (
    <SplitManager onHostPairingsCleared={cleared} />
  ) : (
    <ProfileManager />
  );
  const view = await act(async () =>
    render(<ZMKAppProvider value={current}>{panel}</ZMKAppProvider>),
  );
  return {
    container: view.container,
    current,
    sent,
    cleared,
    recover: () => {
      intercept = false;
    },
  };
}

async function mutate(operation: (typeof mutations)[number]) {
  if (
    operation === "setProfileName" &&
    !screen.queryByPlaceholderText("Device name")
  ) {
    await act(async () => {
      fireEvent.click(screen.getByTitle("Edit name"));
    });
    fireEvent.change(screen.getByPlaceholderText("Device name"), {
      target: { value: "retry-draft" },
    });
  }
  await act(async () => {
    switch (operation) {
      case "setProfileName":
        fireEvent.click(screen.getByRole("button", { name: /Save/ }));
        break;
      case "switchProfile":
        fireEvent.click(
          screen.getByRole("button", { name: /Switch to this profile/ }),
        );
        break;
      case "unpairProfile":
        fireEvent.click(screen.getByRole("button", { name: /Unpair/ }));
        break;
      case "setOutputPriority":
        fireEvent.click(screen.getByRole("button", { name: /Bluetooth/ }));
        break;
      case "forgetSplitBond":
        fireEvent.click(
          screen.getByRole("button", { name: /Clear All Host Pairings/ }),
        );
        break;
    }
  });
}

it.each(
  reads.flatMap((operation) => boundaries.map((b) => [operation, b] as const)),
)(
  "%s admits only its single matching response: %s",
  async (operation, boundary) => {
    const fixture = await start(operation, boundary);
    const error = fixture.container.querySelector(".error-message");
    if (boundary === "matching") {
      expect(error).toBeNull();
    } else {
      // A visible operation error is the regression assertion, not a timeout.
      expect(error).not.toBeNull();
      expect(error).toHaveTextContent(/Failed to/);
      fixture.recover();
      await act(async () => {
        fireEvent.click(screen.getByRole("button", { name: /Refresh/ }));
      });
      expect(fixture.container.querySelector(".error-message")).toBeNull();
    }
    expect(screen.getByRole("button", { name: /Refresh/ })).toBeEnabled();
    if (operation === "getProfiles")
      expect(screen.getByText("fixture-host")).toBeInTheDocument();
    else if (operation === "getOutputPriority")
      expect(
        screen.getByText("USB", { selector: ".priority-badge" }),
      ).toBeInTheDocument();
    else expect(screen.getByText("Central")).toBeInTheDocument();
    expect(call_rpc).toHaveBeenCalledTimes(boundary === "matching" ? 1 : 2);
    for (const call of (call_rpc as jest.Mock).mock.calls)
      expect(call[0]).toBe(fixture.current.state.connection);
  },
);

it.each(
  mutations.flatMap((operation) =>
    [...boundaries, "empty acknowledgement" as const].map(
      (boundary) => [operation, boundary] as const,
    ),
  ),
)(
  "%s requires its own explicit success before follow-up: %s",
  async (operation, boundary) => {
    const fixture = await start(operation, boundary);
    await mutate(operation);
    const error = fixture.container.querySelector(".error-message");
    if (boundary === "matching") {
      expect(error).toBeNull();
    } else {
      expect(error).not.toBeNull();
      expect(error).toHaveTextContent(/Failed to/);
      // An invalid reply cannot issue a success follow-up or retire the draft.
      expect(fixture.sent).toHaveLength(2);
      expect(fixture.cleared).not.toHaveBeenCalled();
      expect(window.alert).not.toHaveBeenCalled();
      if (operation === "setProfileName") {
        expect(screen.getByPlaceholderText("Device name")).toHaveValue(
          "retry-draft",
        );
        expect(screen.getByPlaceholderText("Device name")).toBeEnabled();
      }
      fixture.recover();
      await mutate(operation);
      expect(fixture.container.querySelector(".error-message")).toBeNull();
    }
    expect(fixture.sent).toHaveLength(boundary === "matching" ? 3 : 4);
    expect(screen.getByRole("button", { name: /Refresh/ })).toBeEnabled();
    if (operation === "setProfileName")
      expect(screen.queryByPlaceholderText("Device name")).toBeNull();
    if (operation === "forgetSplitBond") {
      expect(fixture.cleared).toHaveBeenCalledTimes(1);
      expect(window.alert).toHaveBeenCalledTimes(1);
    } else {
      expect(fixture.cleared).not.toHaveBeenCalled();
      if (operation !== "setOutputPriority")
        expect(
          within(fixture.container).getByText("fixture-host"),
        ).toBeInTheDocument();
    }
  },
);
