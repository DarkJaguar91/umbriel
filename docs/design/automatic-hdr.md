# Automatic HDR color feedback

The color-management protocol describes a logical encoding independently of the destination color volume. Umbriel offers a PQ encoding in automatic HDR mode on outputs that can activate HDR, even while their current scanout is SDR. This prevents a startup dependency with clients such as Wine: the client needs HDR headroom before creating HDR content, while physical activation needs that content to arrive first.

`ColorManager::descriptionForOutput` selects the logical description, and both per-output information and preferred surface information use it. An idle automatic HDR output offers BT.2020 PQ with a primary maximum of 10000 cd/m² and the configured SDR reference white. Its target remains sRGB with the nominal SDR luminance range. Active HDR replaces that target with the output's display metadata. Unsupported outputs and disabled HDR policies retain SDR descriptions.

The description objects are immutable. Returning to idle changes the target back to SDR while retaining logical HDR headroom, so a new client can still start HDR. Changing `sdr_white` updates the logical reference white even while scanout remains SDR. Reusing an output or preferred PQ description as surface metadata retains its reference luminance and requests automatic HDR just like a client-created PQ description.

## Capability and activation

Output configuration tests an HDR state through the backend using the display's PQ and BT.2020 support, renderer output transforms and available 10-bit formats. The test uses a separate pending state and can try without VRR. It does not commit that state or change the scene's SDR white level.

Physical activation remains tied to a mapped fullscreen view with committed HDR metadata. The output frame builds the scene buffer and commits it together with the pending HDR encoding and render format. Its pixels therefore use the new encoding from the first presented HDR frame.

A real activation rejection withdraws logical HDR availability even if SDR fallback succeeds. Ordinary idle commits preserve that rejection; clearing a diagnostic fallback reason does not re-advertise HDR. A deliberate output reconfiguration or mode change probes support again. A successful physical HDR commit also restores availability. Retryable scene-build failures do not invalidate previously tested capability.

## SDR fallback

The scene marks HDR sources for SDR mapping when their destination is SDR. This includes PQ, absolute-luminance scRGB and descriptions with luminance headroom. The texture shader decodes and converts straight RGB before applying a continuous Reinhard shoulder above linear 0.8. Scaling all channels together retains their ratios. It then restores premultiplied alpha before blending. Ordinary SDR sources and intermediate textures leave the mapping disabled.

The pending output state selects this policy, so a static source buffer moves between SDR mapping and HDR presentation without requiring a client repaint. Window capture scenes use the same SDR mapping. The additional texture option belongs to UmbrielFX; shared wlroots structure prefixes remain unchanged.

## Verification

`color-description` covers logical headroom across idle, active, returned-idle and unavailable states, truthful destination metadata and reference-white changes. `color-scene-hdr-sdr-fallback` renders SDR/HDR/SDR transitions and checks distinct highlights, PQ/scRGB equivalence, custom luminances and alpha. Headless protocol checks verify that unsupported outputs continue to offer SDR.

Real DRM verification must also cover a fresh Proton startup in automatic mode, fullscreen activation, return to idle, windowed fallback and mixed SDR/HDR outputs. Headless outputs cannot establish successful hardware activation.
