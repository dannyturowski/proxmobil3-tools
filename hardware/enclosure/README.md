# PROXmobil3 enclosure v18

Editable CadQuery source, STEP assembly, and six printable STL parts for the experimental back enclosure and right-side control sponson. Original design by whim.ski.

## Files and rebuild

- `case_v18.py`: editable construction source, in millimetres.
- `source/enclosure-v17.step`: retained input geometry required by that script.
- `enclosure-v18.step`: six-part assembly export.
- `print-parts/`: back cover, right sponson, service lid, two access tunnels, and battery retainer.

Install Python/CadQuery using `requirements.txt`, then run `python case_v18.py <output-directory>`. Use a separate output directory when comparing the shipped export. The publication copy changes only the input path to a repository-relative path; it does not redesign the part.

The files are a design iteration, **not a universally validated drop-in enclosure**. Existing prints establish that parts have been fabricated; they do not establish complete assembly fit.

## Assembly concept

The battery rests against the rear panel and is captured by a removable side cantilever. Flush access tunnels reach the battery rocker and PROXdongle button. The sponson holds a vertical ToF board, a front-facing toggle above it, and two side tactile switches. A removable service lid retains the switches. Two rear M6 insert positions support suction cups. Four corner screws attach the cover to the reader.

Nominal envelopes used in the CAD: battery 145 x 85 x 28 mm; toggle body 30 x 15 x 18 mm plus a 9.2 mm terminal envelope; tact switches 6 x 6 x 7 mm. These are variant-specific assumptions, not a substitute for checking your parts. The outer corner radius is 20 mm, with concentric mating-lip reliefs.

## Known fit issues and checks before printing

- **Corner mounting screws:** the user reported M4 screws longer than expected were needed and 30 mm screws did not protrude far enough. This public snapshot does not claim that the screw-seat depth has been corrected. Check grip length, head seating, engagement and blind-hole depth before printing or ordering hardware. Do not force an overlong screw into the reader.
- Toggle barrel/key fit, terminal boots, wiring bends and actual right-angle USB plug clearance require a wired dry-fit.
- Keep the ToF optical path clear. A close-fitting printed window and a working bare sensor do not prove that the enclosure does not clip its view.
- The retained input and current exported assembly have passed historical digital solid/interference checks. Any rebuild validation performed for publication is documented separately; physical fit is still a separate test.
- The included photographs show earlier bottom-mounted cradles, not this side-sponson revision.

## Fasteners

Sponson-to-case: two M3 thread-forming screws at 51 mm pitch into 2.5 mm pilots. Sensor: rear-accessible M2 pilots. Service lid: M2.5 thread-formers. Each access tunnel: one M3 countersunk screw. Battery retainer: two M3 thread-formers. Rear M6 inserts use nominal 8 mm through-bores; match actual insert outside diameter and length. Screw lengths remain dependent on the actual assembly and are intentionally not guessed here.

Print orientation and support settings are not included as a tested universal profile. Existing project plates/settings remain private working artifacts; use the STEP/STLs to prepare a profile for your printer and material.
