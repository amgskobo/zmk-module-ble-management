import { Response } from "./proto/zmk/ble_management/ble_management";

// The generated codec keeps oneof members as separate optional properties.
// Admit exactly one result for this operation before changing panel state or
// issuing a mutation's follow-up read. An empty mutation message is a refusal:
// its schema's explicit success bool defaults to false.
export function decodeResponse<K extends Exclude<keyof Response, "error">>(
  payload: Uint8Array | null | undefined,
  operation: K
): NonNullable<Response[K]> {
  if (!payload) throw new Error("No BLE management response received");
  const response = Response.decode(payload);
  if (
    Object.values(response).filter((value) => value !== undefined).length !== 1
  )
    throw new Error("Unexpected BLE management response");
  if (response.error)
    throw new Error(response.error.message.trim() || "Request refused");
  const result = response[operation];
  if (!result) throw new Error("Unexpected BLE management response");
  return result as NonNullable<Response[K]>;
}
