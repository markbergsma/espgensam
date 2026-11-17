# Passive GLM bus capture

Use listen-only mode to observe traffic from the official GLM adapter without transmitting
from espgensam.

## Prepare

1. Copy `secrets.yaml.example` to `secrets.yaml` and fill in the credentials.
2. Select the board connected to the bus and set `listen_only: "true"` in `espgensam-local.yaml`.
3. Connect the controller's RS-485 receiver to the bus. Keep the wiring and termination
   appropriate for the selected board; see [the transceiver comparison](rs485-transceiver-comparison.md).
4. Confirm the startup log reports listen-only mode before asking GLM to perform any action.

## Capture

Create a descriptive filename and stream the logs while performing one controlled action in
GLM:

```bash
mkdir -p captures
esphome logs espgensam.yaml | tee captures/glm-descriptive-action.log
```

Stop the log stream with Ctrl-C. Check that the capture contains no espgensam `TX ->` records.
Record the monitor models, firmware, GLM action and approximate time alongside any conclusions;
change only one setting per comparison where possible.

The `captures/` directory and the local configuration files are gitignored. Raw captures and
`.sam` setups can contain device identifiers and private setup data; review and sanitize them
before sharing. Restore the intended `listen_only` setting after the capture.