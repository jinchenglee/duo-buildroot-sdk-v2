# TinyTag training for fewer expensive false ROI proposals

Status: training proposal and decision record, updated 2026-09-30. The mixed
six-channel fine-tuning code implements positive box-area cost alongside mined
empty-proposal cost in `tinytag_mixed_finetune.py`; the notebook records the
experiment settings. `tools/tinytag_cvimodel/` converts and validates trained
models.

Both training from random initialization and fine-tuning an existing checkpoint
are supported by this proposal. Synthetic ground truth and the cost-weighted
negative objective do not require an existing trained model. The frozen-teacher
term is optional and applies when a useful checkpoint is available.

## Objective and priority

Network inference is already fast enough. The priority is to reduce the CPU
time spent decoding false ROI proposals while preserving detection of tags
within the supported operating range. Keep the existing six-channel model:
coverage mask, center heatmap, offsets, and sizes. Cutting output channels is
not the current objective.

Better training is the first effort. A recursive Bayesian filter on the mask,
blob/tag tracking, and adaptive crop decoding are secondary mechanisms. They
can stabilize predictions or schedule work, but persistent background mistakes
can survive temporal filtering and acquire stronger historical support.

Count both false proposals and their decoder cost. One large empty crop can
cost more than several small crops. Evaluate decoder time at matched tag recall,
not proposal precision alone.

## Supported tag size and oversized proposals

Generate training tags only within a declared recoverable size and quality
range. Tags below the physical resolution limit do not need to contribute to
the recall target. Avoid generating these examples rather than spending model
capacity trying to recover information the camera image does not contain.

Determine the cutoff with the actual camera-resolution image, lens, blur,
contrast, projection angle, and decoder. A necessary geometric constraint such
as ArUco Nano's current 10-pixel minimum candidate side is not a guarantee of
readability. Account for the narrowest projected side and pixels per tag cell,
not only bounding-box area. Small in the 640x360 network input does not
necessarily mean unrecoverable in the 1280x720 decoder input. Likewise, a tag
unreadable at half size may still belong to the supported full-resolution range.

For synthetic generation, reject placements outside the supported range after
the final transformations. For real data containing unsupported tiny tags,
retain an ignore annotation around them. Do not erase their annotations and
then train their pixels as confidently tag-free background. Report supported
and ignored tag counts separately. Retain difficult but recoverable positives
close to the cutoff so training does not silently sacrifice boundary recall.

A huge proposal containing only a tiny tag should be rare with correctly trained
coverage and box outputs. Treat it primarily as a training, labeling, or ROI
generation defect to investigate. Check mask bridges, excessive coverage,
oversized box predictions, margins, and temporal growth. This exceptional case
should not drive the main design. Nevertheless, a proposal containing a
supported real tag must not receive an empty-proposal label merely because its
box has poor IoU with that tag. Correct excessive crop extent through coverage
and localization supervision, including a direct cost on predicted positive-box
area. The existing containment and IoU terms preserve localization, but do not
directly express the operational objective: fewer pixels passed to the crop
decoder. A measured 971 comparison found that the fine-tuned model produced
about one third as many ROIs as V4C while total crop pixels stayed similar; its
mean crop area per ROI was about 2.9 times larger. A follow-up comparison used
the proposal-first decoder and the same 1.25x expansion on both models, with all mask growth disabled: it
reduced mean proposals from 13.34 to 4.04 per frame and crop pixels from 94,383
to 43,196 per frame (54% lower). Its average crop was still about 1.5 times
larger than V4C's, while decoded recall was 88.9% versus 91.4% (217 versus 223
of 244 tags). This makes box area an independent optimization target even after
bounded decoding removes much of the excess crop area.

## Synthetic data with known ground truth

Composite generated AprilTag 36h11 tags onto varied ImageNet backgrounds and
backgrounds from the target camera environment. Ground truth for inserted tags
includes ID, transformed quadrilateral, visible coverage, and the targets for
center/offset/size outputs. The backgrounds themselves must be free of target
tags, or any pre-existing tags need annotation or exclusion.

Include background-only images, single tags, nearby tags, and multiple tags.
Otherwise training can learn that every image must contain a tag. Include
reviewed hard negatives from real recordings: textured surfaces, repeated
patterns, printed graphics, and other regions the current model proposes.
Decoder failure alone is insufficient to label a real proposal negative.

Render at the camera resolution, then apply the detector's crop and downscale
to derive the network input. Apply matching transformations to annotations.
Vary perspective, scale, lens distortion, illumination, tag/background contrast,
defocus and motion blur, sensor noise, occlusion, and image-boundary clipping.
Apply optical and sensor degradations to the composite so pasted tags do not
remain artificially sharp against degraded backgrounds. Keep the final input
consistent with the production monochrome/luma path.

Represent tag coverage as geometric tag support, not only its black bit pixels.
Document how visible coverage, partially covered grid cells, occlusion, and
quiet-zone margins become mask targets. Decoder crop padding and the mask label
need not be identical. Avoid a background penalty that inadvertently removes
the margin the decoder needs.

Split source backgrounds and real recording sessions into training and
validation sets before generating composites. Keep real held-out recordings
for the final acceptance test. Synthetic variation and real-data fine-tuning
are supported by general object-detection research, but our recipe still needs
validation on this camera and tag detector [1].

## Cost-weighted false-proposal loss

Start with hard-negative mining using the actual deployed ROI extraction:
mask thresholds, connected regions, center peaks, box expansion, and crop
padding. A small number of false mask cells can form a bridge that creates a
large bounding box; ordinary pixel-error counts do not capture that cost.

For each training image, extract proposals from the current student or a recent
mining checkpoint. A proposal that contains or partially overlaps a supported
tag is not wholly treated as positive: exclude supported-tag cells from its
negative region and continue mining the remaining empty cells. Exclude ignored
or uncertain cells the same way. Do not use a low box IoU alone to declare a
proposal empty. Initially use reviewed cases with clearly no tag in the crop,
and refresh the mined proposals as the model changes.

At the model grid resolution, let `R_j` be the proposal's cells that have no
supported-tag coverage and are not ignored/uncertain, and let `A_j` be the
proposal's full-resolution decoder crop area. If no eligible cells remain, the
proposal contributes no false-proposal loss. Define its cost weight as:

```text
w_j = clip(A_j / A_ref, w_min, w_max)
```

Area is the first cost approximation. Later, replace it with an estimate of
measured decoder time, since contour complexity also matters. Use baseline
full-resolution crop cost for the initial comparison, so adaptive routing does
not obscure the cost of the network's proposals.

`A_ref` only sets the weight scale. An illustrative 40,000-pixel reference gives
weights of 4 for a 400x400 crop and 0.25 for a 100x100 crop before clipping. It
is not an ROI eligibility gate or a calibrated constant. Caps stop an enormous
crop from dominating the batch.

For student mask probability `p_i` in grid cell `i`, the regional penalty is:

```text
L_false,j = stop_gradient(w_j) * mean over i in R_j [-log(1 - p_i)]
L_false   = sum of regional penalties / training batch size
```

A proposal may overlap a real tag and still incur this penalty on its empty
cells; cells covered by a supported tag's expanded training box are excluded.
The current implementation makes this exclusion at stride-8 grid-cell
resolution, so any cell touched by the supported box is exempt. Ignored and
uncertain regions are also excluded cell by cell. This preserves positive mask
supervision while encouraging the proposal mask to avoid unnecessary empty
area around a tag.

For a known empty region, the target is zero. Before cost weighting, a
probability of 0.9 incurs loss 2.30, 0.5 incurs 0.69, and 0.1 incurs 0.11.
Confident mistakes on expensive crops receive more emphasis. Averaging within
the crop prevents counting crop area twice. Keep a fixed batch normalization;
normalizing away all cost weights would defeat the intended emphasis. The
precise reduction, clipping, and overlap handling require experiments.

This mined-empty term alone cannot optimize the size of a crop that contains a
supported tag: those proposal cells are excluded from false-proposal penalties,
and proposal geometry is detached during mining. Add a separate differentiable
positive-box area term, using each supervised target crop as the minimum
supported area:

```text
A_pred   = predicted_width * predicted_height
A_target = target_width * target_height
L_area   = mean_positive [relu(A_pred - A_target) / A_target]
L_total  = L_positive + lambda_false * L_false + lambda_area * L_area
```

The positive containment penalty remains active and penalizes any predicted
edge that excludes target area; the area term therefore charges only avoidable
oversize. It acts directly on box width and height, unlike `L_false`, which
changes mask probability and only influences crop creation indirectly. When a
fixed expansion ratio is applied at inference to both predicted and target
boxes, their area ratio is unchanged. Keep the area weight separately
configurable and validate recall and summed crop pixels on held-out real data;
the initial notebook value is an experiment setting, not a calibrated constant.

Treat proposal selection and its area as fixed for the step. Backpropagation
flows through the selected mask probabilities, not through connected components
or box extraction. Detaching the cost prevents a direct gradient incentive to
shrink the predicted box instead of rejecting background. Refreshing mined
proposals is still necessary to detect later changes in proposal shape.

Mask suppression is essential for the current ROI generator: removing a center
peak can leave a mask blob that still creates a proposal. A background loss for
the center heatmap can supplement the mask penalty using its own valid negative
targets. Box regression remains supervised at positive locations; arbitrary
background boxes have no meaningful regression target.

Ordinary per-cell background training can remain at lower weight alongside
mined expensive mistakes. Focal loss is an optional way to reduce the influence
of easy negatives; it addresses class imbalance, while cost weighting addresses
decoder work [2]. Neither is established as the best choice for this model.

## Why false-proposal-only fine-tuning is risky

Stopping gradients from the original detection losses does not freeze detection
behavior. Shared weights affect both positives and negatives. A model that
proposes nothing minimizes a false-proposal-only objective. Monitoring recall
or the old losses reveals regressions but does not prevent them.

With only `lambda * L_false`, lambda mainly scales the optimization update. It
does not express a precision-versus-recall tradeoff. Freezing the backbone and
box head, and updating only mask/center heads, limits changes but still permits
those heads to suppress real tags.

## Positive preservation with a frozen teacher

The teacher is a frozen copy of the starting checkpoint. The student starts
from the same weights and receives updates. Both see the same augmented input.
Use ground truth to select true-tag regions; do not imitate the teacher across
the whole image, which would preserve its false positives.

For a teacher mask probability `t_i` and student probability `p_i`, one
preservation term is:

```text
L_preserve,mask = mean over supported positive cells P [
    t_i * log(t_i / p_i)
  + (1 - t_i) * log((1 - t_i) / (1 - p_i))
]
```

This is the KL divergence between two Bernoulli outputs. It is zero when the
student agrees with the teacher and grows as they differ. Implement it with
numerically stable operations; the expression above explains the objective.

For sigmoid logits, the negative cross-entropy term pushes according to `p_i`;
the teacher-preservation term pushes according to `p_i - t_i`:

| Situation | Training effect |
| --- | --- |
| Empty background, student 0.8 | Push mask confidence down |
| True tag, teacher 0.95, student 0.3 | Push student confidence back up |
| True tag, teacher 0.95, student 0.95 | No corrective preservation gradient |

Preservation can also cover the center response at supported true-tag locations
and offset/size predictions where positive geometry targets are defined. A
scaled Smooth L1 term can preserve geometry. Define and normalize these terms
separately; mask, center, and geometry losses have different scales.

Teacher agreement is not ground-truth correctness. A teacher that gives low
confidence to a difficult real tag provides weak protection there. Therefore
retain supervised positive targets for supported tags, especially near the
recoverability cutoff.

## Fine-tuning objective and first experiments

For fine-tuning an existing checkpoint, the combined objective is:

```text
L_total = lambda * L_false
        + mu     * L_preserve
        + nu     * L_positive
```

`L_false` reduces expensive empty proposals. `L_preserve` discourages regression
from the current checkpoint. `L_positive` trains against actual positive ground
truth and can reuse the positive portions of the existing mask, center, and box
losses. The preservation term is an alternative safeguard, not a way to avoid
all positive gradients.

Start with the simpler objective: existing positive supervision plus
cost-weighted hard negatives (`mu = 0`). Then test whether teacher preservation
adds value. Compare against the current checkpoint, ordinary fine-tuning on
the same new data, and a cautious false-proposal-only run. Use the same data and
evaluation rules to distinguish the effect of the loss from better examples.

Choose weights from loss/gradient scales and held-out results; there are no
validated numerical coefficients yet. Do not let cheap gains on abundant easy
background overwhelm recall for supported difficult tags.

## Training from scratch

Initialize the network randomly and train the backbone and all detection heads.
Do not freeze randomly initialized features or heads. There is no useful teacher
at this point, so omit `L_preserve` (`mu = 0`). Use synthetic ground truth to
learn both tag responses and background rejection:

```text
L_supervised = a * L_mask
             + b * L_center
             + c * L_geometry

L_total = L_supervised + lambda(t) * L_false
```

`L_mask` covers supported tag and background cells, using the declared coverage
targets and ignoring unsupported/uncertain regions. `L_center` includes valid
tag centers and background locations, with the chosen center-target convention.
`L_geometry` supervises offsets and sizes at positive locations only. Balance
the classification terms so the many background cells do not dominate. The
coefficients `a`, `b`, and `c` depend on the reductions and target conventions;
no numerical values are validated here.

Unlike the fine-tuning experiment, positive supervision alone plus penalties on
mined false proposals is not the preferred starting objective. An untrained
network needs ordinary background supervision even where it does not currently
produce a mined proposal. Keep the complete supervised losses throughout
training; the cost term adds emphasis to expensive mistakes. Document that
background cells in mined regions receive both ordinary supervision and the
additional cost penalty, and scale the terms deliberately.

Suggested sequence:

1. Warm up with `lambda(t) = 0`, using synthetic positive images and
   background-only images. Train all heads until held-out proposals have
   meaningful coverage and geometry. Select the transition from validation
   behavior rather than an assumed number of epochs.
2. Extract proposals from a recent student checkpoint using the deployed ROI
   generator. Mine empty crops from known backgrounds, respecting ignore
   annotations. No pre-existing trained model is needed.
3. Increase the cost weight gradually while retaining all supervised losses.
   Refresh the mined proposals as the student changes. Random early masks and
   boxes can form huge arbitrary crops, so do not let a large initial cost
   penalty dominate learning before useful detection is established.
4. Select checkpoints using supported-tag recall and measured decoder cost on
   held-out data. Validate the exported INT8 model and real camera recordings.

Teacher preservation is optional later: a useful checkpoint from the warm-up
can become the frozen teacher for subsequent optimization. It is not required
for the from-scratch route and must not replace ground-truth supervision.

For an A/B comparison, train the same architecture from scratch on the same
data with ordinary supervision versus supervision plus the cost term. Compare
at matched recall using the same inference thresholds and ROI extraction.
Include initialization variation in the comparison so one favorable random
seed is not mistaken for a loss-function improvement. A currently deployed
checkpoint can be an additional reference if available, not a prerequisite.

## Acceptance and measurements

Evaluate the exported INT8 model with the actual ROI generator and decoder on
identical frames. Keep the default full-resolution path as the initial baseline;
measure adaptive decoding separately. Record:

- Supported-tag recall per frame and by size/quality band, plus first-detection
  delay on sequences. Excluded tiny tags are outside this recall denominator.
- Empty proposals and empty crop pixels per frame, including their upper tails.
- Crop count/area, excess area around supported tags, and decoder stage timings.
- Mean and tail pipeline processing time, decoded IDs, and corner quality.

Compare decoder time at matched recall. Review regressions individually rather
than relying on aggregate marker counts. Oversized crops around tiny supported
tags should be reported as defects, even if rare. An improved training loss is
not proof of improved runtime or maintained recall.

## References and related decisions

1. [Training Deep Networks with Synthetic Data: Bridging the Reality Gap by
   Domain Randomization](https://arxiv.org/abs/1804.06516). General support for
   synthetic variation and real-data fine-tuning; not a validation of this tag
   training proposal.
2. [Focal Loss for Dense Object Detection](https://arxiv.org/abs/1708.02002).
   Reduces domination by easy negatives in dense detection.

- [Adaptive ROI decoding rules](tinytag-adaptive-roi-rules.md): runtime scheduling,
  provisional area gate, and A/B recall requirements.
- [Point LDC status](ldc-point-correction.md): distortion handling and missing-quad
  limitations that affect whether a proposed crop can decode.
