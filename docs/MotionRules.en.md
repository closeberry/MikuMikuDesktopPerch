# Rules for generated motions

[日本語](MotionRules.md)

The motions MikuMikuDesktopPerch generates, or layers on top, in the program follow the basic conventions of MMD motions.
This keeps them usable as they are in other models and tools when written out as VMD.

Bone names are given in Japanese as they appear in standard MMD models, with English in parentheses.

## Constraints per bone

- **全ての親 (mother)**: not moved.
- **センター (center)**: no Y-axis rotation; the body's direction is changed by rotating 上半身 and 下半身. No up-and-down movement either. Movement along X and Z may be used, and when the model travels (walking and the like), it travels with センター.
- **グルーブ (groove)**: only up-and-down movement is used.
- **ひじ (elbow)**: only rotation about the local Y axis is used. It does not bend about the local Z axis. Twist about the local X axis is expressed with 腕捩 (arm twist).
- **手首 (wrist)**: rotation about the local X axis is not used. Twist is expressed with 手捩 (wrist twist).

## Local axes and twist

- When the model does not set local axes, they are computed from the skeleton (the bones' positions and connections).
- On models without twist bones (腕捩, 手捩), the twist restrictions above do not apply.
- Generated motions do not use twist directly. Any twist needed is left to **automatic twist distribution** (redistributing twist components arising in the elbow and wrist to 腕捩 and 手捩).

## How the implementation (`src/GeneratedMotion.*`) keeps them

- Bones written: センター (X movement only), グルーブ (Y movement only), 下半身 (lower body), 上半身 (upper body) and 首 (neck) (rotation), the left and right 足ＩＫ (leg IK) (movement and rotation), and 腕 (arm), ひじ, 腕捩 and the fingers other than the thumb (rotation)
- While generating, so that the idle VMD's tilt does not remain, センター's rotation, 上半身2 (upper body 2), 頭 (head), 肩 (shoulder), 手首, 手捩 and つま先ＩＫ (toe IK) are held at the rest pose (肩 turns only as under "shoulder" below, when a gesture moves the arm; 手首 and 手捩 only in the gestures under "hand orientation" below)
- Arm swings and elbow bends are made only from rotation about a "bend forward" axis found from the bone's direction and the front direction. The model's local axes are used only when their X axis roughly matches the bone's direction
- The elbow's rotation goes through automatic twist distribution (`DistributeTwist`) last. The component about local X moves to 腕捩, and of the rest only the component about local Y is kept (local Z is dropped)
- When walking, センター advances, and the 足ＩＫ of a planted foot is fixed in space. 全ての親 is never moved
- The tilt of the foot (raising the heel, raising the toes) is given by the 足ＩＫ's rotation. Its position is moved too, so it turns about the toes when raising the heel and about the heel when raising the toes (so the toes and heel do not sink into the ground)
- Shifting weight while idle is expressed by センター's X movement (the hips over one foot) and the Z rotation of 下半身 and 上半身. The 足ＩＫ stay fixed
- Idle gestures (a hand on the hip, waving, a peace sign and so on) set 腕 and ひじ with two-joint IK from the wrist's target position. The arm is also turned along its length to match the plane the elbow bends in; the elbow turns only about its bend axis, and the wrist is normally not rotated
- In gestures where the hand's orientation matters (palms facing each other in a clap, hands lowered and held together in front), after the arm and elbow are set, the hand's turn is found from target directions for the fingers and the palm; the part about the forearm's length (twist) goes to 手捩 and the rest to 手首 (the wrist does not turn about its local X axis; on a model without 手捩, all of it goes to 手首)
- In gestures where palms meet (a clap), the palm's surface, not the wrist, is put on the target point. Each model's palm surface is found from where the hand's own vertices (those weighted mostly to the wrist) reach farthest on the palm side. Whatever the shoulder width, arm length, standing pose (T-stance or A-stance) or hand thickness, the palms neither stay apart nor go through each other
- **肩 (shoulder)**: when a gesture moves the arm (腕 and ひじ set with two-joint IK), the shoulder is turned by the same formula as MotionTraceEditor's automatic shoulder. The elbow's offset from where it hangs naturally is divided by the upper arm's length; the rise times 42° (a drop times 14°), the forward move (plus 0.65 of a move toward the body's middle, 0.35 of one away) times 26° (backward times 18°). At most 50° up, 15° down and 20° either way. Turning the shoulder moves the arm's root, so the arm and elbow are solved again from there and the hand still reaches its target. Over a motion the user registered (an idle motion and the like), the shoulder is left as the motion has it, and the arm and elbow are solved from that shoulder
- Click reactions are made with the same mechanism as gestures, layered on top of the standing pose, gestures and idle VMD. The bones written and the constraints are the same as for gestures
- When a walk or run VMD given by the user (one stepping in place) is used, turning sideways is done the same way as the generated walk: by rotating the 足ＩＫ's position and rotation about the vertical axis, and adding rotation to 下半身, 上半身 and 首. The VMD's up and down of センター is moved to グルーブ, and センター keeps only X and Z movement. The speed of travel is found from how fast a planted 足ＩＫ slides backward
- Expressions (feelings in talks) and dozing are made in the same layer on top as blinking, breathing and gaze. Expressions use only morphs; dozing uses only the blink morph and the rotation of 首, 頭 and 上半身
- When sitting, the thighs go down at a slant (raising the knees tends to go through the skirt). The body is turned sideways by 下半身's Y rotation, and turns back to the front with 上半身 and 首 (センター is not rotated)
- When hanging and when crouching for a jump too, the body is tilted only by rotating 上半身 and 下半身, and its height is changed only with グルーブ (センター is neither rotated nor moved up or down)
- The up-and-down movement of jumps and falls is not included in the motion; it is expressed by moving the window
- When hanging (a chin-up), the 腕 bone is turned along its length so the elbow bends upward. The elbow only rotates about its bend axis, without twist
- While generating, the fingers are not left straight but curled a little (a little more from the index to the little finger). Gestures and resting poses set their own curl over this
- The fingers other than the thumb are rotated only about the axis that bends toward the palm (the direction joining the bases of the index and little fingers)
- The warp's spin and shrink are display-only transforms, and are not written to センター or 全ての親
- When turning to face the other character in a chat between characters, 下半身 and 上半身 (when it is not a child of 下半身) are rotated about Y, and the positions of the left and right 足ＩＫ (and the つま先ＩＫ that are not children of 足ＩＫ) are rotated about the vertical axis around the point between the feet, with the same amount added to their rotation. センター and 全ての親 are not rotated
- When looking into the other character's eyes in a chat, 首 and 頭 are raised or lowered, in the same layer on top as gaze, to match the direction from the character's own eyes to the other's (the position of the 左目 / 右目 (left / right eye) bones)

## Relation to existing corrections

- The T-stance correction (`MascotModel`'s corrective arm rotation) is a display correction that absorbs differences in the model's rest pose, and is not included in the motion data itself.
