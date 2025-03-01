//=============================================================================
//  MuseScore
//  Music Composition & Notation
//
//  Copyright (C) 2002-2016 Werner Schweer
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2
//  as published by the Free Software Foundation and appearing in
//  the file LICENCE.GPL
//=============================================================================

#include "accidental.h"
#include "barline.h"
#include "beam.h"
#include "box.h"
#include "chord.h"
#include "clef.h"
#include "element.h"
#include "fingering.h"
#include "glissando.h"
#include "harmony.h"
#include "key.h"
#include "keysig.h"
#include "layoutbreak.h"
#include "layout.h"
#include "lyrics.h"
#include "marker.h"
#include "measure.h"
#include "mscore.h"
#include "notedot.h"
#include "note.h"
#include "ottava.h"
#include "page.h"
#include "part.h"
#include "repeat.h"
#include "score.h"
#include "segment.h"
#include "sig.h"
#include "slur.h"
#include "staff.h"
#include "stem.h"
#include "style.h"
#include "sym.h"
#include "system.h"
#include "text.h"
#include "tie.h"
#include "timesig.h"
#include "tremolo.h"
#include "tuplet.h"
#include "undo.h"
#include "utils.h"
#include "volta.h"
#include "breath.h"
#include "tempotext.h"
#include "systemdivider.h"
#include "hook.h"
#include "ambitus.h"
#include "hairpin.h"

namespace Ms {

// #define PAGE_DEBUG

#ifdef PAGE_DEBUG
#define PAGEDBG(...)  qDebug(__VA_ARGS__)
#else
#define PAGEDBG(...)  ;
#endif

//---------------------------------------------------------
//   rebuildBspTree
//---------------------------------------------------------

void Score::rebuildBspTree()
      {
      for (Page* page : _pages)
            page->rebuildBspTree();
      }

//---------------------------------------------------------
//   searchNote
//    search for note or rest before or at tick position tick
//    in staff
//---------------------------------------------------------

ChordRest* Score::searchNote(int tick, int track) const
      {
      ChordRest* ipe = 0;
      Segment::Type st = Segment::Type::ChordRest;
      for (Segment* segment = firstSegment(st); segment; segment = segment->next1(st)) {
            ChordRest* cr = segment->cr(track);
            if (!cr)
                  continue;
            if (cr->tick() == tick)
                  return cr;
            if (cr->tick() >  tick)
                  return ipe ? ipe : cr;
            ipe = cr;
            }
      return 0;
      }

//---------------------------------------------------------
//   layoutChords1
//    - layout upstem and downstem chords
//    - offset as necessary to avoid conflict
//---------------------------------------------------------

void Score::layoutChords1(Segment* segment, int staffIdx)
      {
      Staff* staff = Score::staff(staffIdx);

      if (staff->isTabStaff())
            return;

      std::vector<Note*> upStemNotes;
      std::vector<Note*> downStemNotes;
      int upVoices       = 0;
      int downVoices     = 0;
      int startTrack     = staffIdx * VOICES;
      int endTrack       = startTrack + VOICES;
      qreal nominalWidth = noteHeadWidth() * staff->mag();
      qreal maxUpWidth   = 0.0;
      qreal maxDownWidth = 0.0;
      qreal maxUpMag     = 0.0;
      qreal maxDownMag   = 0.0;

      // dots and hooks can affect layout of notes as well as vice versa
      int upDots         = 0;
      int downDots       = 0;
      bool upHooks       = false;
      bool downHooks     = false;

      // also check for grace notes
      bool upGrace       = false;
      bool downGrace     = false;

      for (int track = startTrack; track < endTrack; ++track) {
            Element* e = segment->element(track);
            if (e && e->isChord()) {
                  Chord* chord = toChord(e);
                  bool hasGraceBefore = false;
                  for (Chord* c : chord->graceNotes()) {
                        if (c->isGraceBefore())
                              hasGraceBefore = true;
                        layoutChords2(c->notes(), c->up());       // layout grace note noteheads
                        layoutChords3(c->notes(), staff, 0);      // layout grace note chords
                        }
                  if (chord->up()) {
                        ++upVoices;
                        upStemNotes.insert(upStemNotes.end(), chord->notes().begin(), chord->notes().end());
                        upDots   = qMax(upDots, chord->dots());
                        maxUpMag = qMax(maxUpMag, chord->mag());
                        if (!upHooks)
                              upHooks = chord->hook();
                        if (hasGraceBefore)
                              upGrace = true;
                        }
                  else {
                        ++downVoices;
                        downStemNotes.insert(downStemNotes.end(), chord->notes().begin(), chord->notes().end());
                        downDots = qMax(downDots, chord->dots());
                        maxDownMag = qMax(maxDownMag, chord->mag());
                        if (!downHooks)
                              downHooks = chord->hook();
                        if (hasGraceBefore)
                              downGrace = true;
                        }
                  }
            }

      if (upVoices + downVoices) {
            // TODO: use track as secondary sort criteria?
            // otherwise there might be issues with unisons between voices
            // in some corner cases

            maxUpWidth   = nominalWidth * maxUpMag;
            maxDownWidth = nominalWidth * maxDownMag;

            // layout upstem noteheads
            if (upVoices > 1) {
                  qSort(upStemNotes.begin(), upStemNotes.end(),
                     [](Note* n1, const Note* n2) ->bool {return n1->line() > n2->line(); } );
                  }
            if (upVoices) {
                  qreal hw = layoutChords2(upStemNotes, true);
                  maxUpWidth = qMax(maxUpWidth, hw);
                  }

            // layout downstem noteheads
            if (downVoices > 1) {
                  qSort(downStemNotes.begin(), downStemNotes.end(),
                     [](Note* n1, const Note* n2) ->bool {return n1->line() > n2->line(); } );
                  }
            if (downVoices) {
                  qreal hw = layoutChords2(downStemNotes, false);
                  maxDownWidth = qMax(maxDownWidth, hw);
                  }

            qreal sp                 = staff->spatium();
            qreal upOffset           = 0.0;      // offset to apply to upstem chords
            qreal downOffset         = 0.0;      // offset to apply to downstem chords
            qreal dotAdjust          = 0.0;      // additional chord offset to account for dots
            qreal dotAdjustThreshold = 0.0;      // if it exceeds this amount

            // centering adjustments for whole note, breve, and small chords
            qreal centerUp          = 0.0;      // offset to apply in order to center upstem chords
            qreal oversizeUp        = 0.0;      // adjustment to oversized upstem chord needed if laid out to the right
            qreal centerDown        = 0.0;      // offset to apply in order to center downstem chords
            qreal centerAdjustUp    = 0.0;      // adjustment to upstem chord needed after centering donwstem chord
            qreal centerAdjustDown  = 0.0;      // adjustment to downstem chord needed after centering upstem chord

            // only center chords if they differ from nominal by at least this amount
            // this avoids unnecessary centering on differences due only to floating point roundoff
            // it also allows for the possibility of disabling centering
            // for notes only "slightly" larger than nominal, like half notes
            // but this will result in them not being aligned with each other between voices
            // unless you change to left alignment as described in the comments below
            qreal centerThreshold   = 0.01 * sp;

            // amount by which actual width exceeds nominal, adjusted for staff mag() only
            qreal headDiff = maxUpWidth - nominalWidth;
            // amount by which actual width exceeds nominal, adjusted for staff & chord/note mag()
            qreal headDiff2 = maxUpWidth - nominalWidth * (maxUpMag / staff->mag());
            if (headDiff > centerThreshold) {
                  // larger than nominal
                  centerUp = headDiff * -0.5;
                  // maxUpWidth is true width, but we no longer will care about that
                  // instead, we care only about portion to right of origin
                  maxUpWidth += centerUp;
                  // to left align rather than center, delete both of the above
                  if (headDiff2 > centerThreshold) {
                        // if max notehead is wider than nominal with chord/note mag() applied
                        // then noteheads extend to left of origin
                        // because stemPosX() is based on nominal width
                        // so we need to correct for that too
                        centerUp += headDiff2;
                        oversizeUp = headDiff2;
                        }
                  }
            else if (-headDiff > centerThreshold) {
                  // smaller than nominal
                  centerUp = -headDiff * 0.5;
                  if (headDiff2 > centerThreshold) {
                        // max notehead is wider than nominal with chord/note mag() applied
                        // perform same adjustment as above
                        centerUp += headDiff2;
                        oversizeUp = headDiff2;
                        }
                  centerAdjustDown = centerUp;
                  }

            headDiff = maxDownWidth - nominalWidth;
            if (headDiff > centerThreshold) {
                  // larger than nominal
                  centerDown = headDiff * -0.5;
                  // to left align rather than center, change the above to
                  //centerAdjustUp = headDiff;
                  maxDownWidth = nominalWidth - centerDown;
                  }
            else if (-headDiff > centerThreshold) {
                  // smaller than nominal
                  centerDown = -headDiff * 0.5;
                  centerAdjustUp = centerDown;
                  }

            // handle conflict between upstem and downstem chords

            if (upVoices && downVoices) {
                  Note* bottomUpNote = upStemNotes.front();
                  Note* topDownNote  = downStemNotes.back();
                  int separation;
                  if (bottomUpNote->chord()->staffMove() == topDownNote->chord()->staffMove())
                        separation = topDownNote->line() - bottomUpNote->line();
                  else
                        separation = 2;   // no conflict
                  QVector<Note*> overlapNotes;
                  overlapNotes.reserve(8);

                  if (separation == 1) {
                        // second
                        downOffset = maxUpWidth;
                        // align stems if present, leave extra room if not
                        if (topDownNote->chord()->stem() && bottomUpNote->chord()->stem())
                              downOffset -= topDownNote->chord()->stem()->lineWidth();
                        else
                              downOffset += 0.1 * sp;
                        }

                  else if (separation < 1) {

                        // overlap (possibly unison)

                        // build list of overlapping notes
                        for (int i = 0, n = upStemNotes.size(); i < n; ++i) {
                              if (upStemNotes[i]->line() >= topDownNote->line() - 1)
                                    overlapNotes.append(upStemNotes[i]);
                              else
                                    break;
                              }
                        for (int i = downStemNotes.size() - 1; i >= 0; --i) {
                              if (downStemNotes[i]->line() <= bottomUpNote->line() + 1)
                                    overlapNotes.append(downStemNotes[i]);
                              else
                                    break;
                              }
                        qSort(overlapNotes.begin(), overlapNotes.end(),
                           [](Note* n1, const Note* n2) ->bool {return n1->line() > n2->line(); } );

                        // determine nature of overlap
                        bool shareHeads = true;       // can all overlapping notes share heads?
                        bool matchPending = false;    // looking for a unison match
                        bool conflictUnison = false;  // unison found
                        bool conflictSecondUpHigher = false;      // second found
                        bool conflictSecondDownHigher = false;    // second found
                        int lastLine = 1000;
                        Note* p = overlapNotes[0];
                        for (int i = 0, count = overlapNotes.size(); i < count; ++i) {
                              Note* n = overlapNotes[i];
                              NoteHead::Type nHeadType;
                              NoteHead::Type pHeadType;
                              Chord* nchord = n->chord();
                              Chord* pchord = p->chord();
                              if (n->mirror()) {
                                    if (separation < 0) {
                                          // don't try to share heads if there is any mirroring
                                          shareHeads = false;
                                          // don't worry about conflicts involving mirrored notes
                                          continue;
                                          }
                                    }
                              int line = n->line();
                              int d = lastLine - line;
                              switch (d) {
                                    case 0:
                                          // unison
                                          conflictUnison = true;
                                          matchPending = false;
                                          nHeadType = (n->headType() == NoteHead::Type::HEAD_AUTO) ? n->chord()->durationType().headType() : n->headType();
                                          pHeadType = (p->headType() == NoteHead::Type::HEAD_AUTO) ? p->chord()->durationType().headType() : p->headType();
                                          // the most important rules for sharing noteheads on unisons between voices are
                                          // that notes must be one same line with same tpc
                                          // noteheads must be unmirrored and of same group
                                          // and chords must be same size (or else sharing code won't work)
                                          if (n->headGroup() != p->headGroup() || n->tpc() != p->tpc() || n->mirror() || p->mirror() || nchord->small() != pchord->small()) {
                                                shareHeads = false;
                                                }
                                          else {
                                                // noteheads are potentially shareable
                                                // it is more subjective at this point
                                                // current default is to require *either* of the following:
                                                //    1) both chords have same number of dots, both have stems, and both noteheads are same type and are full size (automatic match)
                                                // or 2) one or more of the noteheads is not of type AUTO, but is explicitly set to match the other (user-forced match)
                                                // or 3) exactly one of the noteheads is invisible (user-forced match)
                                                // thus user can force notes to be shared despite differing number of dots or either being stemless
                                                // by setting one of the notehead types to match the other or by making one notehead invisible
                                                // TODO: consider adding a style option, staff properties, or note property to control sharing
                                                if ((nchord->dots() != pchord->dots() || !nchord->stem() || !pchord->stem() || nHeadType != pHeadType || n->small() || p->small()) &&
                                                    ((n->headType() == NoteHead::Type::HEAD_AUTO && p->headType() == NoteHead::Type::HEAD_AUTO) || nHeadType != pHeadType) &&
                                                    (n->visible() == p->visible())) {
                                                      shareHeads = false;
                                                      }
                                                }
                                          break;
                                    case 1:
                                          // second
                                          // trust that this won't be a problem for single unison
                                          if (separation < 0) {
                                                if (n->chord()->up())
                                                      conflictSecondUpHigher = true;
                                                else
                                                      conflictSecondDownHigher = true;
                                                shareHeads = false;
                                                }
                                          break;
                                    default:
                                          // no conflict
                                          if (matchPending)
                                                shareHeads = false;
                                          matchPending = true;
                                    }
                              p = n;
                              lastLine = line;
                              }
                        if (matchPending)
                              shareHeads = false;

                        // calculate offsets
                        if (shareHeads) {
                              for (int i = overlapNotes.size() - 1; i >= 1; i -= 2) {
                                    Note* p = overlapNotes[i-1];
                                    Note* n = overlapNotes[i];
                                    if (!(p->chord()->isNudged() || n->chord()->isNudged())) {
                                          if (p->chord()->dots() == n->chord()->dots()) {
                                                // hide one set dots
                                                bool onLine = !(p->line() & 1);
                                                if (onLine) {
                                                      // hide dots for lower voice
                                                      if (p->voice() & 1)
                                                            p->setDotsHidden(true);
                                                      else
                                                            n->setDotsHidden(true);
                                                      }
                                                else {
                                                      // hide dots for upper voice
                                                      if (!(p->voice() & 1))
                                                            p->setDotsHidden(true);
                                                      else
                                                            n->setDotsHidden(true);
                                                      }
                                                }
                                          // formerly we hid noteheads in an effort to fix playback
                                          // but this doesn't work for cases where noteheads cannot be shared
                                          // so better to solve the problem elsewhere
                                          }
                                    }
                              }
                        else if (conflictUnison && separation == 0 && (!downGrace || upGrace))
                              downOffset = maxUpWidth + 0.3 * sp;
                        else if (conflictUnison)
                              upOffset = maxDownWidth + 0.3 * sp;
                        else if (conflictSecondUpHigher)
                              upOffset = maxDownWidth + 0.2 * sp;
                        else if ((downHooks && !upHooks) && !(upDots && !downDots))
                              downOffset = maxUpWidth + 0.3 * sp;
                        else if (conflictSecondDownHigher) {
                              if (downDots && !upDots)
                                    downOffset = maxUpWidth + 0.3 * sp;
                              else {
                                    upOffset = maxDownWidth - 0.2 * sp;
                                    if (downHooks)
                                          upOffset += 0.3 * sp;
                                    }
                              }
                        else {
                              // no direct conflict, so parts can overlap (downstem on left)
                              // just be sure that stems clear opposing noteheads
                              qreal clearLeft = 0.0, clearRight = 0.0;
                              if (topDownNote->chord()->stem())
                                    clearLeft = topDownNote->chord()->stem()->lineWidth() + 0.3 * sp;
                              if (bottomUpNote->chord()->stem())
                                    clearRight = bottomUpNote->chord()->stem()->lineWidth() + qMax(maxDownWidth - maxUpWidth, 0.0) + 0.3 * sp;
                              else
                                    downDots = 0; // no need to adjust for dots in this case
                              upOffset = qMax(clearLeft, clearRight);
                              if (downHooks) {
                                    // we will need more space to avoid collision with hook
                                    // but we won't need as much dot adjustment
                                    upOffset = qMax(upOffset, maxDownWidth + 0.1 * sp);
                                    dotAdjustThreshold = maxUpWidth - 0.3 * sp;
                                    }
                              // if downstem chord is small, don't center
                              // and we might not need as much dot adjustment either
                              if (centerDown > 0.0) {
                                    centerDown = 0.0;
                                    centerAdjustUp = 0.0;
                                    dotAdjustThreshold = (upOffset - maxDownWidth) + maxUpWidth - 0.3 * sp;
                                    }
                              }

                        }

                  // adjust for dots
                  if ((upDots && !downDots) || (downDots && !upDots)) {
                        // only one sets of dots
                        // place between chords
                        int dots;
                        qreal mag;
                        if (upDots) {
                              dots = upDots;
                              mag = maxUpMag;
                              }
                        else {
                              dots = downDots;
                              mag = maxDownMag;
                              }
                        qreal dotWidth = segment->symWidth(SymId::augmentationDot);
                        // first dot
                        dotAdjust = styleP(StyleIdx::dotNoteDistance) + dotWidth;
                        // additional dots
                        if (dots > 1)
                              dotAdjust += styleP(StyleIdx::dotDotDistance) * (dots - 1);
                        dotAdjust *= mag;
                        // only by amount over threshold
                        dotAdjust = qMax(dotAdjust - dotAdjustThreshold, 0.0);
                        }
                  if (separation == 1)
                        dotAdjust += 0.1 * sp;

                  }

            // apply chord offsets
            for (int track = startTrack; track < endTrack; ++track) {
                  Element* e = segment->element(track);
                  if (e && e->isChord()) {
                        Chord* chord = toChord(e);
                        if (chord->up()) {
                              if (upOffset != 0.0) {
                                    chord->rxpos() += upOffset + centerAdjustUp + oversizeUp;
                                    if (downDots && !upDots)
                                          chord->rxpos() += dotAdjust;
                                    }
                              else
                                    chord->rxpos() += centerUp;
                              }
                        else {
                              if (downOffset != 0.0) {
                                    chord->rxpos() += downOffset + centerAdjustDown;
                                    if (upDots && !downDots)
                                          chord->rxpos() += dotAdjust;
                                    }
                              else
                                    chord->rxpos() += centerDown;
                              }
                        }
                  }

            // layout chords
            std::vector<Note*> notes;
            if (upVoices)
                  notes.insert(notes.end(), upStemNotes.begin(), upStemNotes.end());
            if (downVoices)
                  notes.insert(notes.end(), downStemNotes.begin(), downStemNotes.end());
            if (upVoices + downVoices > 1)
                  qSort(notes.begin(), notes.end(),
                     [](Note* n1, const Note* n2) ->bool {return n1->line() > n2->line(); } );
            layoutChords3(notes, staff, segment);
            }

      for (int track = startTrack; track < endTrack; ++track) {
            Element* e = segment->element(track);
            if (e)
                  e->layout();
            }
      }

//---------------------------------------------------------
//   layoutChords2
//    - determine which notes need mirroring
//    - this is called once for each stem direction
//      eg, once for voices 1&3, once for 2&4
//      with all notes combined and sorted to resemble one chord
//    - return maximum non-mirrored notehead width
//---------------------------------------------------------

qreal Score::layoutChords2(std::vector<Note*>& notes, bool up)
      {
      int startIdx, endIdx, incIdx;
      qreal maxWidth = 0.0;

      // loop in correct direction so that first encountered notehead wins conflict
      if (up) {
            // loop bottom up
            startIdx = 0;
            endIdx = notes.size();
            incIdx = 1;
            }
      else {
            // loop top down
            startIdx = notes.size() - 1;
            endIdx = -1;
            incIdx = -1;
            }

      int ll        = 1000;         // line of previous notehead
                                    // hack: start high so first note won't show as conflict
      bool lvisible = false;        // was last note visible?
      bool mirror   = false;        // should current notehead be mirrored?
                                    // value is retained and may be used on next iteration
                                    // to track mirror status of previous note
      bool isLeft   = notes[startIdx]->chord()->up();             // is notehead on left?
      int lmove     = notes[startIdx]->chord()->staffMove();      // staff offset of last note (for cross-staff beaming)

      for (int idx = startIdx; idx != endIdx; idx += incIdx) {
            Note* note    = notes[idx];                     // current note
            int line      = note->line();                   // line of current note
            Chord* chord  = note->chord();
            int move      = chord->staffMove();             // staff offset of current note

            // there is a conflict
            // if this is same or adjacent line as previous note (and chords are on same staff!)
            // but no need to do anything about it if either note is invisible
            bool conflict = (qAbs(ll - line) < 2) && (lmove == move) && note->visible() && lvisible;

            // this note is on opposite side of stem as previous note
            // if there is a conflict
            // or if this the first note *after* a conflict
            if (conflict || (chord->up() != isLeft))
                  isLeft = !isLeft;

            // determine if we would need to mirror current note
            // to get it to the correct side
            // this would be needed to get a note to left or downstem or right of upstem
            // whether or not we actually do this is determined later (based on user mirror property)
            bool nmirror = (chord->up() != isLeft);

            // by default, notes and dots are not hidden
            // this may be changed later to allow unisons to share noteheads
            note->setHidden(false);
            note->setDotsHidden(false);

            // be sure chord position is initialized
            // chord may be moved to the right later
            // if there are conflicts between voices
            chord->rxpos() = 0.0;

            // let user mirror property override the default we calculated
            if (note->userMirror() == MScore::DirectionH::AUTO) {
                  mirror = nmirror;
                  }
            else {
                  mirror = note->chord()->up();
                  if (note->userMirror() == MScore::DirectionH::LEFT)
                        mirror = !mirror;
                  }
            note->setMirror(mirror);

            // accumulate return value
            if (!mirror)
                  maxWidth = qMax(maxWidth, note->headWidth());

            // prepare for next iteration
            lvisible = note->visible();
            lmove    = move;
            ll       = line;
            }

      return maxWidth;
      }

//---------------------------------------------------------
//   AcEl
//---------------------------------------------------------

struct AcEl {
      Note* note;
      qreal x;          // actual x position of this accidental relative to origin
      qreal top;        // top of accidental bbox relative to staff
      qreal bottom;     // bottom of accidental bbox relative to staff
      int line;         // line of note
      int next;         // index of next accidental of same pitch class (ascending list)
      qreal width;      // width of accidental
      qreal ascent;     // amount (in sp) vertical strokes extend above body
      qreal descent;    // amount (in sp) vertical strokes extend below body
      qreal rightClear; // amount (in sp) to right of last vertical stroke above body
      qreal leftClear;  // amount (in sp) to left of last vertical stroke below body
      };

//---------------------------------------------------------
//   resolveAccidentals
//    lx = calculated position of rightmost edge of left accidental relative to origin
//---------------------------------------------------------

static bool resolveAccidentals(AcEl* left, AcEl* right, qreal& lx, qreal pd, qreal sp)
      {
      AcEl* upper;
      AcEl* lower;
      if (left->line >= right->line) {
            upper = right;
            lower = left;
            }
      else {
            upper = left;
            lower = right;
            }

      qreal gap = lower->top - upper->bottom;

      // no conflict at all if there is sufficient vertical gap between accidentals
      // the arrangement of accidentals into columns assumes accidentals an octave apart *do* clear
      if (gap >= pd || lower->line - upper->line >= 7)
            return false;

      qreal allowableOverlap = qMax(upper->descent, lower->ascent) - pd;

      // accidentals that are "close" (small gap or even slight overlap)
      if (qAbs(gap) <= 0.33 * sp) {
            // acceptable with slight offset
            // if one of the accidentals can subsume the overlap
            // and both accidentals allow it
            if (-gap <= allowableOverlap && qMin(upper->descent, lower->ascent) > 0.0) {
                  qreal align = qMin(left->width, right->width);
                  lx = qMin(lx, right->x + align - pd);
                  return true;
                  }
            }

      // amount by which overlapping accidentals will be separated
      // for example, the vertical stems of two flat signs
      // these need more space than we would need between non-overlapping accidentals
      qreal overlapShift = pd * 1.41;

      // accidentals with more significant overlap
      // acceptable if one accidental can subsume overlap
      if (left == lower && -gap <= allowableOverlap) {
            qreal offset = qMax(left->rightClear, right->leftClear);
            offset = qMin(offset, left->width) - overlapShift;
            lx = qMin(lx, right->x + offset);
            return true;
            }

      // accidentals with even more overlap
      // can work if both accidentals can subsume overlap
      if (left == lower && -gap <= upper->descent + lower->ascent - pd) {
            qreal offset = qMin(left->rightClear, right->leftClear) - overlapShift;
            if (offset > 0.0) {
                  lx = qMin(lx, right->x + offset);
                  return true;
                  }
            }

      // otherwise, there is real conflict
      lx = qMin(lx, right->x - pd);
      return true;
      }

//---------------------------------------------------------
//   layoutAccidental
//---------------------------------------------------------

static qreal layoutAccidental(AcEl* me, AcEl* above, AcEl* below, qreal colOffset, QVector<Note*>& leftNotes, qreal pnd, qreal pd, qreal sp)
      {
      qreal lx = colOffset;
      Accidental* acc = me->note->accidental();
      qreal mag = acc->mag();
      pnd *= mag;
      pd *= mag;

      // extra space for ledger lines
      if (me->line <= -2 || me->line >= me->note->staff()->lines() * 2)
            lx = qMin(lx, -0.2 * sp);

      // clear left notes
      int lns = leftNotes.size();
      for (int i = 0; i < lns; ++i) {
            Note* ln = leftNotes[i];
            int lnLine = ln->line();
            qreal lnTop = (lnLine - 1) * 0.5 * sp;
            qreal lnBottom = lnTop + sp;
            if (me->top - lnBottom <= pnd && lnTop - me->bottom <= pnd) {
                  // undercut note above if possible
                  if (lnBottom - me->top <= me->ascent - pnd)
                        lx = qMin(lx, ln->x() + ln->chord()->x() + me->rightClear);
                  else
                        lx = qMin(lx, ln->x() + ln->chord()->x());
                  }
            else if (lnTop > me->bottom)
                  break;
            }

      // clear other accidentals
      bool conflictAbove = false;
      bool conflictBelow = false;

      if (above)
            conflictAbove = resolveAccidentals(me, above, lx, pd, sp);
      if (below)
            conflictBelow = resolveAccidentals(me, below, lx, pd, sp);
      if (conflictAbove || conflictBelow)
            me->x = lx - acc->width() - acc->bbox().x();
      else if (colOffset != 0.0)
            me->x = lx - pd - acc->width() - acc->bbox().x();
      else
            me->x = lx - pnd - acc->width() - acc->bbox().x();

      return me->x;
      }

//---------------------------------------------------------
//   layoutChords3
//    - calculate positions of notes, accidentals, dots
//---------------------------------------------------------

void Score::layoutChords3(std::vector<Note*>& notes, Staff* staff, Segment* segment)
      {
      //---------------------------------------------------
      //    layout accidentals
      //    find column for dots
      //---------------------------------------------------

      QVector<Note*> leftNotes; // notes to left of origin
      leftNotes.reserve(8);
      QVector<AcEl> aclist;       // accidentals
      aclist.reserve(8);

      // track columns of octave-separated accidentals
      int columnBottom[7] = { -1, -1, -1, -1, -1, -1, -1 };

      qreal sp           = staff->spatium();
      qreal stepDistance = sp * staff->logicalLineDistance() * .5;
      int stepOffset     = staff->staffType()->stepOffset();

      qreal lx                = 10000.0;  // leftmost notehead position
      qreal upDotPosX         = 0.0;
      qreal downDotPosX       = 0.0;

      int nNotes = notes.size();
      int nAcc = 0;
      for (int i = nNotes-1; i >= 0; --i) {
            Note* note     = notes[i];
            Accidental* ac = note->accidental();
            if (ac && !note->fixed()) {
                  ac->layout();
                  AcEl acel;
                  acel.note   = note;
                  int line    = note->line();
                  acel.line   = line;
                  acel.x      = 0.0;
                  acel.top    = line * 0.5 * sp + ac->bbox().top();
                  acel.bottom = line * 0.5 * sp + ac->bbox().bottom();
                  acel.width  = ac->width();
                  QPointF bboxNE = ac->symBbox(ac->symbol()).topRight();
                  QPointF bboxSW = ac->symBbox(ac->symbol()).bottomLeft();
                  QPointF cutOutNE = ac->symCutOutNE(ac->symbol());
                  QPointF cutOutSW = ac->symCutOutSW(ac->symbol());
                  if (!cutOutNE.isNull()) {
                        acel.ascent     = cutOutNE.y() - bboxNE.y();
                        acel.rightClear = bboxNE.x() - cutOutNE.x();
                        }
                  else {
                        acel.ascent     = 0.0;
                        acel.rightClear = 0.0;
                        }
                  if (!cutOutSW.isNull()) {
                        acel.descent   = bboxSW.y() - cutOutSW.y();
                        acel.leftClear = cutOutSW.x() - bboxSW.x();
                        }
                  else {
                        acel.descent   = 0.0;
                        acel.leftClear = 0.0;
                        }
                  int pitchClass = (line + 700) % 7;
                  acel.next = columnBottom[pitchClass];
                  columnBottom[pitchClass] = nAcc;
                  aclist.append(acel);
                  ++nAcc;
                  }

            qreal hw     = note->headWidth();   // actual head width, including note & chord mag
            Chord* chord = note->chord();
            bool _up     = chord->up();
            qreal stemX  = chord->stemPosX();   // stem position for nominal notehead, but allowing for mag

            qreal overlapMirror;
            Stem* stem = chord->stem();
            if (stem)
                  overlapMirror = stem->lineWidth();
            else if (chord->durationType().headType() == NoteHead::Type::HEAD_WHOLE)
                  overlapMirror = styleP(StyleIdx::stemWidth) * chord->mag();
            else
                  overlapMirror = 0.0;

            qreal x;
            if (note->mirror()) {
                  if (_up)
                        x = stemX - overlapMirror;
                  else
                        x = stemX - hw + overlapMirror;
                  }
            else {
                  if (_up)
                        x = stemX - hw;
                  else
                        x = 0.0;
                  }

            note->rypos()  = (note->line() + stepOffset) * stepDistance;
            note->rxpos()  = x;
            // we need to do this now
            // or else note pos / readPos / userOff will be out of sync
            // and we rely on note->x() throughout the layout process
            note->adjustReadPos();

            // find leftmost non-mirrored note to set as X origin for accidental layout
            // a mirrored note that extends to left of segment X origin
            // will displace accidentals only if there is conflict
            qreal sx = x + chord->x(); // segment-relative X position of note
            if (note->mirror() && !chord->up() && sx < 0.0)
                  leftNotes.append(note);
            else if (sx < lx)
                  lx = sx;

            qreal xx = x + hw + chord->pos().x();

            Direction dotPosition = note->userDotPosition();
            if (chord->dots()) {
                  if (chord->up())
                        upDotPosX = qMax(upDotPosX, xx);
                  else
                        downDotPosX = qMax(downDotPosX, xx);

                  if (dotPosition == Direction::AUTO && nNotes > 1 && note->visible() && !note->dotsHidden()) {
                        // resolve dot conflicts
                        int line = note->line();
                        Note* above = (i < nNotes - 1) ? notes[i+1] : 0;
                        if (above && (!above->visible() || above->dotsHidden()))
                              above = 0;
                        int intervalAbove = above ? line - above->line() : 1000;
                        Note* below = (i > 0) ? notes[i-1] : 0;
                        if (below && (!below->visible() || below->dotsHidden()))
                              below = 0;
                        int intervalBelow = below ? below->line() - line : 1000;
                        if ((line & 1) == 0) {
                              // line
                              if (intervalAbove == 1 && intervalBelow != 1)
                                    dotPosition = Direction::DOWN;
                              else if (intervalBelow == 1 && intervalAbove != 1)
                                    dotPosition = Direction::UP;
                              else if (intervalAbove == 0 && above->chord()->dots()) {
                                    // unison
                                    if (((above->voice() & 1) == (note->voice() & 1))) {
                                          above->setDotY(Direction::UP);
                                          dotPosition = Direction::DOWN;
                                          }
                                    }
                              }
                        else {
                              // space
                              if (intervalAbove == 0 && above->chord()->dots()) {
                                    // unison
                                    if (!(note->voice() & 1))
                                          dotPosition = Direction::UP;
                                    else {
                                          if (!(above->voice() & 1))
                                                above->setDotY(Direction::UP);
                                          else
                                                dotPosition = Direction::DOWN;
                                          }
                                    }
                              }
                        }
                  }
            note->setDotY(dotPosition);  // also removes invalid dots
            }

      if (segment) {
            // align all dots for segment/staff
            // it would be possible to dots for up & down chords separately
            // this would require space to have been allocated previously
            // when calculating chord offsets
            segment->setDotPosX(staff->idx(), qMax(upDotPosX, downDotPosX));
            }

      if (nAcc == 0)
            return;

      QVector<int> umi;
      qreal pd  = styleP(StyleIdx::accidentalDistance);
      qreal pnd = styleP(StyleIdx::accidentalNoteDistance);
      qreal colOffset = 0.0;

      if (nAcc >= 2 && aclist[nAcc-1].line - aclist[0].line >= 7) {

            // accidentals spread over an octave or more
            // set up columns for accidentals with octave matches
            // these will start at right and work to the left
            // unmatched accidentals will use zig zag approach (see below)
            // starting to the left of the octave columns

            qreal minX = 0.0;
            int columnTop[7] = { -1, -1, -1, -1, -1, -1, -1 };

            // find columns of octaves
            for (int pc = 0; pc < 7; ++pc) {
                  if (columnBottom[pc] == -1)
                        continue;
                  // calculate column height
                  for (int j = columnBottom[pc]; j != -1; j = aclist[j].next)
                        columnTop[pc] = j;
                  }

            // compute reasonable column order
            // use zig zag
            QVector<int> column;
            QVector<int> unmatched;
            int n = nAcc - 1;
            for (int i = 0; i <= n; ++i, --n) {
                  int pc = (aclist[i].line + 700) % 7;
                  if (aclist[columnTop[pc]].line != aclist[columnBottom[pc]].line) {
                        if (!column.contains(pc))
                              column.append(pc);
                        }
                  else
                        unmatched.append(i);
                  if (i == n)
                        break;
                  pc = (aclist[n].line + 700) % 7;
                  if (aclist[columnTop[pc]].line != aclist[columnBottom[pc]].line) {
                        if (!column.contains(pc))
                              column.append(pc);
                        }
                  else
                        unmatched.append(n);
                  }
            int nColumns = column.size();
            int nUnmatched = unmatched.size();

            // handle unmatched accidentals
            for (int i = 0; i < nUnmatched; ++i) {
                  // first try to slot it into an existing column
                  AcEl* me = &aclist[unmatched[i]];
                  // find column
                  bool found = false;
                  for (int j = 0; j < nColumns; ++j) {
                        int pc = column[j];
                        int above = -1;
                        int below = -1;
                        // find slot within column
                        for (int k = columnBottom[pc]; k != -1; k = aclist[k].next) {
                              if (aclist[k].line < me->line) {
                                    above = k;
                                    break;
                                    }
                              below = k;
                              }
                        // check to see if accidental can fit in slot
                        qreal myPd = pd * me->note->accidental()->mag();
                        bool conflict = false;
                        if (above != -1 && me->top - aclist[above].bottom < myPd)
                              conflict = true;
                        else if (below != -1 && aclist[below].top - me->bottom < myPd)
                              conflict = true;
                        if (!conflict) {
                              // insert into column
                              found = true;
                              me->next = above;
                              if (above == -1)
                                    columnTop[pc] = unmatched[i];
                              if (below != -1)
                                    aclist[below].next = unmatched[i];
                              else
                                    columnBottom[pc] = unmatched[i];
                              break;
                              }
                        }
                  // if no slot found, then add to list of unmatched accidental indices
                  if (!found)
                        umi.push_back(unmatched[i]);
                  }
            nAcc = umi.size();
            if (nAcc > 1)
                  qSort(umi);

            // lay out columns
            for (int i = 0; i < nColumns; ++i) {
                  int pc = column[i];
                  AcEl* below = 0;
                  // lay out accidentals
                  for (int j = columnBottom[pc]; j != -1; j = aclist[j].next) {
                        qreal x = layoutAccidental(&aclist[j], 0, below, colOffset, leftNotes, pnd, pd, sp);
                        minX = qMin(minX, x);
                        below = &aclist[j];
                        }
                  // align within column
                  int next = -1;
                  for (int j = columnBottom[pc]; j != -1; j = next) {
                        next = aclist[j].next;
                        if (next != -1 && aclist[j].line == aclist[next].line)
                              continue;
                        aclist[j].x = minX;
                        }
                  // move to next column
                  colOffset = minX;
                  }

            }

      else {
            for (int i = 0; i < nAcc; ++i)
                  umi.push_back(i);
            }

      if (nAcc) {

            // for accidentals with no octave matches, use zig zag approach
            // layout right to left in pairs, (next) highest then lowest

            AcEl* me = &aclist[umi[0]];
            AcEl* above = 0;
            AcEl* below = 0;

            // layout top accidental
            layoutAccidental(me, above, below, colOffset, leftNotes, pnd, pd, sp);

            // layout bottom accidental
            int n = nAcc - 1;
            if (n > 0) {
                  above = me;
                  me = &aclist[umi[n]];
                  layoutAccidental(me, above, below, colOffset, leftNotes, pnd, pd, sp);
                  }

            // layout middle accidentals
            if (n > 1) {
                  for (int i = 1; i < n; ++i, --n) {
                        // next highest
                        below = me;
                        me = &aclist[umi[i]];
                        layoutAccidental(me, above, below, colOffset, leftNotes, pnd, pd, sp);
                        if (i == n - 1)
                              break;
                        // next lowest
                        above = me;
                        me = &aclist[umi[n-1]];
                        layoutAccidental(me, above, below, colOffset, leftNotes, pnd, pd, sp);
                        }
                  }

            }

      for (const AcEl& e : aclist) {
            // even though we initially calculate accidental position relative to segment
            // we must record pos for accidental relative to note,
            // since pos is always interpreted relative to parent
            Note* note = e.note;
            qreal x    = e.x + lx - (note->x() + note->chord()->x());
            note->accidental()->setPos(x, 0);
            note->accidental()->adjustReadPos();
            }
      }

#define beamModeMid(a) (a == Beam::Mode::MID || a == Beam::Mode::BEGIN32 || a == Beam::Mode::BEGIN64)

//---------------------------------------------------------
//   beamGraceNotes
//---------------------------------------------------------

void Score::beamGraceNotes(Chord* mainNote, bool after)
      {
      ChordRest* a1    = 0;      // start of (potential) beam
      Beam* beam       = 0;      // current beam
      Beam::Mode bm = Beam::Mode::AUTO;
      QVector<Chord*> graceNotes = after ? mainNote->graceNotesAfter() : mainNote->graceNotesBefore();

      for (ChordRest* cr : graceNotes) {
            bm = Groups::endBeam(cr);
            if ((cr->durationType().type() <= TDuration::DurationType::V_QUARTER) || (bm == Beam::Mode::NONE)) {
                  if (beam) {
                        beam->layoutGraceNotes();
                        beam = 0;
                        }
                  if (a1) {
                        a1->removeDeleteBeam(false);
                        a1 = 0;
                        }
                  cr->removeDeleteBeam(false);
                  continue;
                  }
            if (beam) {
                  bool beamEnd = bm == Beam::Mode::BEGIN;
                  if (!beamEnd) {
                        cr->removeDeleteBeam(true);
                        beam->add(cr);
                        cr = 0;
                        beamEnd = (bm == Beam::Mode::END);
                        }
                  if (beamEnd) {
                        beam->layoutGraceNotes();
                        beam = 0;
                        }
                  }
            if (!cr)
                  continue;
            if (a1 == 0)
                  a1 = cr;
            else {
                  if (!beamModeMid(bm) && (bm == Beam::Mode::BEGIN)) {
                        a1->removeDeleteBeam(false);
                        a1 = cr;
                        }
                  else {
                        beam = a1->beam();
                        if (beam == 0 || beam->elements().front() != a1) {
                              beam = new Beam(this);
                              beam->setGenerated(true);
                              beam->setTrack(mainNote->track());
                              a1->removeDeleteBeam(true);
                              beam->add(a1);
                              }
                        cr->removeDeleteBeam(true);
                        beam->add(cr);
                        a1 = 0;
                        }
                  }
            }
      if (beam)
            beam->layoutGraceNotes();
      else if (a1)
            a1->removeDeleteBeam(false);
      }

//---------------------------------------------------------
//   layoutSpanner
//    called after dragging a staff
//---------------------------------------------------------

void Score::layoutSpanner()
      {
      int tracks = ntracks();
      for (int track = 0; track < tracks; ++track) {
            for (Segment* segment = firstSegment(); segment; segment = segment->next1()) {
                  if (track == tracks-1) {
                        int n = segment->annotations().size();
                        for (int i = 0; i < n; ++i)
                              segment->annotations().at(i)->layout();
                        }
                  Element* e = segment->element(track);
                  if (e && e->isChord()) {
                        Chord* c = toChord(segment->element(track));
                        c->layoutStem();
                        for (Note* n : c->notes()) {
                              Tie* tie = n->tieFor();
                              if (tie)
                                    tie->layout();
                              for (Spanner* sp : n->spannerFor())
                                    sp->layout();
                              }
                        }
                  }
            }
      rebuildBspTree();
      }

//-------------------------------------------------------------------
//   addSystemHeader
///   Add elements to make this measure suitable as the first measure
///   of a system.
//    The system header can contain a starting BarLine, a Clef,
//    a KeySig and a RepeatBarLine.
//-------------------------------------------------------------------

void Score::addSystemHeader(Measure* m, bool isFirstSystem)
      {
      m->setHasSystemHeader(true);
      const int tick = m->tick();

      int nVisible = 0;
      int staffIdx = 0;

      for (Staff* staff : _staves) {
            // At this time we don't know which staff is visible or not...
            // but let's not create the key/clef if there were no visible before this layout
            // sometimes we will be right, other time it will take another layout to be right...

            if (!m->system()->staff(staffIdx)->show()) {
                  ++staffIdx;
                  continue;
                  }
            ++nVisible;

            KeySig* keysig   = 0;
            Clef*   clef     = 0;
            const int strack = staffIdx * VOICES;

            // we assume that keysigs and clefs are only in the first
            // track (voice 0) of a staff

            KeySigEvent keyIdx = staff->keySigEvent(tick);

            for (Segment* seg = m->first(); seg; seg = seg->next()) {
                  // search only up to the first ChordRest/StartRepeatBarLine
                  if (seg->isType(Segment::Type::ChordRest | Segment::Type::StartRepeatBarLine))
                        break;
                  Element* el = seg->element(strack);
                  if (!el)
                        continue;
                  switch (el->type()) {
                        case Element::Type::KEYSIG:
                              keysig = toKeySig(el);
                              break;
                        case Element::Type::CLEF:
                              clef = toClef(el);
                              clef->setSmall(false);
                              break;
                        default:
                              break;
                        }
                  }
            // keep key sigs in TABs: TABs themselves should hide them
            bool needKeysig = isFirstSystem || styleB(StyleIdx::genKeysig);

            // If we need a Key::C KeySig (which would be invisible) and there is
            // a courtesy key sig, dont create it and switch generated flags.
            // This avoids creating an invisible KeySig which can distort layout.

            KeySig* ksAnnounce = 0;
            if (needKeysig && (keyIdx.key() == Key::C)) {
                  Measure* pm = m->prevMeasure();
                  if (pm && pm->hasCourtesyKeySig()) {
                        Segment* ks = pm->first(Segment::Type::KeySigAnnounce);
                        if (ks) {
                              ksAnnounce = toKeySig(ks->element(strack));
                              if (ksAnnounce) {
                                    needKeysig = false;
                                    if (keysig) {
                                          ksAnnounce->setGenerated(false);
                                          keysig->setGenerated(true);
                                          }
                                    }
                              }
                        }
                  }

            needKeysig = needKeysig && (keyIdx.key() != Key::C || keyIdx.custom() || keyIdx.isAtonal());
            needKeysig = needKeysig || (keysig && !keysig->generated());  // dont remove user modified keysigs

            if (needKeysig) {
                  if (!keysig) {
                        //
                        // create missing key signature
                        //
                        keysig = new KeySig(this);
                        keysig->setKeySigEvent(keyIdx);
                        keysig->setTrack(strack);
                        keysig->setGenerated(true);
                        Segment* seg = m->undoGetSegment(Segment::Type::KeySig, tick);
                        keysig->setParent(seg);
                        undo(new AddElement(keysig));
                        }
                  else {
                        if (!(keysig->keySigEvent() == keyIdx))
                              undo(new ChangeKeySig(keysig, keyIdx, keysig->showCourtesy()));
                        }
                  keysig->layout();       // hide naturals may have changed
                  keysig->segment()->createShape(staffIdx);
                  }
            else if (keysig) {
                  undoRemoveElement(keysig);
                  keysig = 0;
                  }

            StaffType* staffType  = staff->staffType();
            bool showClef         = staffType->genClef() && (isFirstSystem || styleB(StyleIdx::genClef));
            if (showClef) {
                  ClefTypeList cl = staff->clefType(tick);
                  if (!clef) {
                        //
                        // create missing clef
                        //
                        clef = new Clef(this);
                        clef->setTrack(strack);
                        clef->setSmall(false);
                        clef->setGenerated(true);

                        Segment* s = m->undoGetSegment(Segment::Type::Clef, tick);
                        clef->setParent(s);
                        clef->setClefType(cl);
                        undo(new AddElement(clef));
                        clef->layout();
                        s->createShape(staffIdx);
                        }
                  else {
                        if (cl != clef->clefTypeList()) {
                              undo(new ChangeClefType(clef, cl._concertClef, cl._transposingClef));
                              clef->layout();
                              clef->segment()->createShape(staffIdx);
                              }
                        }
                  }
            else {
                  if (clef) {
                        undo(new RemoveElement(clef));
                        if (clef->segment())
                              clef->segment()->createShape(staffIdx);
                        }
                  }
            ++staffIdx;
            }
      m->setStartRepeatBarLine();

      //
      // create systemic barline
      // new behaviour: depends on number of total staves
      // old behaviour: depends on number of visible staves

      BarLine* bl = 0;
      Segment* s = m->findSegment(Segment::Type::BeginBarLine, tick);
      if (s)
            bl = toBarLine(s->element(0));

      int n = score()->nstaves();
      if ((n > 1 && score()->styleB(StyleIdx::startBarlineMultiple)) || (n == 1 && score()->styleB(StyleIdx::startBarlineSingle))) {
            if (!bl) {
                  bl = new BarLine(this);
                  bl->setTrack(0);
                  bl->setGenerated(true);

                  Segment* seg = m->undoGetSegment(Segment::Type::BeginBarLine, tick);
                  bl->setParent(seg);
                  bl->layout();
                  undo(new AddElement(bl));
                  seg->createShapes();
                  }
            bl->setSpan(n);
            }
      else if (bl)
            score()->undoRemoveElement(bl);
      }

//---------------------------------------------------------
//   cautionaryWidth
//    Compute the width of required courtesy of time signature
//    and key signature elements if m were the last measure
//    in a staff.
//    Return hasCourtesy == true if courtesy elements are
//    already present. The value is undefined if no
//    courtesy elements are required.
//---------------------------------------------------------

qreal Score::cautionaryWidth(Measure* m, bool& hasCourtesy)
      {
      hasCourtesy = false;
      if (m == 0)
            return 0.0;
      Measure* nm = m->nextMeasure();
      if (nm == 0 || (m->sectionBreak() && _layoutMode != LayoutMode::FLOAT))
            return 0.0;

      int tick = m->endTick();

      // locate a time sig. in the next measure and, if found,
      // check if it has caut. sig. turned off

      Segment* ns       = nm->findSegment(Segment::Type::TimeSig, tick);
      bool showCourtesy = styleB(StyleIdx::genCourtesyTimesig);

      qreal w = 0.0;
      if (showCourtesy && ns) {
            TimeSig* ts = toTimeSig(ns->element(0));
            if (ts && ts->showCourtesySig()) {
                  qreal leftMargin  = styleP(StyleIdx::timesigLeftMargin);
                  Segment* s = m->findSegment(Segment::Type::TimeSigAnnounce, tick);
                  if (s && s->element(0)) {
                        w = s->element(0)->width() + leftMargin;
                        hasCourtesy = true;
                        }
                  else {
                        ts->layout();
                        w = ts->width() + leftMargin;
                        hasCourtesy = false;
                        }
                  }
            }

      // courtesy key signatures

      showCourtesy = styleB(StyleIdx::genCourtesyKeysig);
      ns           = nm->findSegment(Segment::Type::KeySig, tick);

      qreal wwMax  = 0.0;
      if (showCourtesy && ns) {
            qreal leftMargin = styleP(StyleIdx::keysigLeftMargin);
            for (int staffIdx = 0; staffIdx < _staves.size(); ++staffIdx) {
                  int track = staffIdx * VOICES;

                  KeySig* nks = toKeySig(ns->element(track));

                  if (nks && nks->showCourtesy() && !nks->generated()) {
                        Segment* s  = m->findSegment(Segment::Type::KeySigAnnounce, tick);

                        if (s && s->element(track)) {
                              wwMax = qMax(wwMax, s->element(track)->width() + leftMargin);
                              hasCourtesy = true;
                              }
                        else {
                              nks->layout();
                              wwMax = qMax(wwMax, nks->width() + leftMargin);
                              //hasCourtesy = false;
                              }
                        }
                  }
            }
      w += wwMax;

      return w;   //* 1.5
      }

//---------------------------------------------------------
//   hideEmptyStaves
//---------------------------------------------------------

void Score::hideEmptyStaves(System* system, bool isFirstSystem)
      {
      int staves   = _staves.size();
      int staffIdx = 0;
      bool systemIsEmpty = true;

      for (Staff* staff : _staves) {
            SysStaff* ss  = system->staff(staffIdx);
//            bool oldShow = ss->show();

            Staff::HideMode hideMode = staff->hideWhenEmpty();

            if (hideMode == Staff::HideMode::ALWAYS
                || (styleB(StyleIdx::hideEmptyStaves)
                    && (staves > 1)
                    && !(isFirstSystem && styleB(StyleIdx::dontHideStavesInFirstSystem))
                    && hideMode != Staff::HideMode::NEVER)) {
                  bool hideStaff = true;
                  for (MeasureBase* m : system->measures()) {
                        if (!m->isMeasure())
                              continue;
                        Measure* measure = toMeasure(m);
                        if (!measure->isMeasureRest(staffIdx)) {
                              hideStaff = false;
                              break;
                              }
                        }
                  // check if notes moved into this staff
                  Part* part = staff->part();
                  int n = part->nstaves();
                  if (hideStaff && (n > 1)) {
                        int idx = part->staves()->front()->idx();
                        for (int i = 0; i < part->nstaves(); ++i) {
                              int st = idx + i;

                              foreach (MeasureBase* mb, system->measures()) {
                                    if (!mb->isMeasure())
                                          continue;
                                    Measure* m = toMeasure(mb);
                                    for (Segment* s = m->first(Segment::Type::ChordRest); s; s = s->next(Segment::Type::ChordRest)) {
                                          for (int voice = 0; voice < VOICES; ++voice) {
                                                ChordRest* cr = s->cr(st * VOICES + voice);
                                                if (cr == 0 || cr->isRest())
                                                      continue;
                                                int staffMove = cr->staffMove();
                                                if (staffIdx == st + staffMove) {
                                                      hideStaff = false;
                                                      break;
                                                      }
                                                }
                                          }
                                    if (!hideStaff)
                                          break;
                                    }
                              if (!hideStaff)
                                    break;
                              }
                        }
                  ss->setShow(hideStaff ? false : staff->show());
                  if (ss->show())
                        systemIsEmpty = false;
                  }
            else {
                  systemIsEmpty = false;
                  ss->setShow(true);
                  }

#if 0
            if (oldShow != s->show()) {
                  foreach (MeasureBase* mb, system->measures()) {
                        if (!mb->isMeasure())
                              continue;
                        static_cast<Measure*>(mb)->createEndBarLines();
                        }
                  }
#endif
            ++staffIdx;
            }
      if (systemIsEmpty) {
            for (Staff* staff : _staves) {
                  SysStaff* ss  = system->staff(staff->idx());
                  if (staff->showIfEmpty() && !ss->show()) {
                        ss->setShow(true);
                        systemIsEmpty = false;
                        }
                  }
            }
      // dont allow a complete empty system
      if (systemIsEmpty) {
            Staff* staff = _staves.front();
            SysStaff* ss = system->staff(staff->idx());
            ss->setShow(true);
            }
      }

//---------------------------------------------------------
//   addPage
//---------------------------------------------------------

Page* Score::addPage()
      {
      Page* page = new Page(this);
      page->setNo(_pages.size());
      _pages.push_back(page);
      return page;
      }

//---------------------------------------------------------
//   connectTies
///   Rebuild tie connections.
//---------------------------------------------------------

void Score::connectTies(bool silent)
      {
      int tracks = nstaves() * VOICES;
      Measure* m = firstMeasure();
      if (!m)
            return;
      Segment::Type st = Segment::Type::ChordRest;
      for (Segment* s = m->first(st); s; s = s->next1(st)) {
            for (int i = 0; i < tracks; ++i) {
                  Chord* c = static_cast<Chord*>(s->element(i));
                  if (c == 0 || !c->isChord())
                        continue;

                  for (Note* n : c->notes()) {
                        // connect a tie without end note
                        Tie* tie = n->tieFor();
                        if (tie && !tie->endNote()) {
                              Note* nnote;
                              if (_mscVersion <= 114)
                                    nnote = searchTieNote114(n);
                              else
                                    nnote = searchTieNote(n);
                              if (nnote == 0) {
                                    if (!silent) {
                                          qDebug("next note at %d track %d for tie not found (version %d)", s->tick(), i, _mscVersion);
                                          delete tie;
                                          n->setTieFor(0);
                                          }
                                    }
                              else {
                                    tie->setEndNote(nnote);
                                    nnote->setTieBack(tie);
                                    }
                              }
                        // connect a glissando without initial note (old glissando format)
                        for (Spanner* spanner : n->spannerBack()) {
                              if (spanner->isGlissando() && !spanner->startElement()) {
                                    Note* initialNote = Glissando::guessInitialNote(n->chord());
                                    n->removeSpannerBack(spanner);
                                    if (initialNote) {
                                          spanner->setStartElement(initialNote);
                                          spanner->setEndElement(n);
                                          spanner->setTick(initialNote->chord()->tick());
                                          spanner->setTick2(n->chord()->tick());
                                          spanner->setTrack(n->track());
                                          spanner->setTrack2(n->track());
                                          spanner->setParent(initialNote);
                                          initialNote->add(spanner);
                                          }
                                    else {
                                          delete spanner;
                                          }
                                    }
                              }
                        // spanner with no end element can happen during copy/paste
                        for (Spanner* spanner : n->spannerFor()) {
                              if (spanner->endElement() == nullptr) {
                                    n->removeSpannerFor(spanner);
                                    delete spanner;
                                    }
                              }
                        }
                  // connect two note tremolos
                  Tremolo* tremolo = c->tremolo();
                  if (tremolo && tremolo->twoNotes() && !tremolo->chord2()) {
                        for (Segment* ls = s->next1(st); ls; ls = ls->next1(st)) {
                              Chord* nc = static_cast<Chord*>(ls->element(i));
                              if (nc == 0)
                                    continue;
                              if (!nc->isChord())
                                    qDebug("cannot connect tremolo");
                              else {
                                    nc->setTremolo(tremolo);
                                    tremolo->setChords(c, nc);
                                    // cross-measure tremolos are not supported
                                    // but can accidentally result from copy & paste
                                    // remove them now
                                    if (c->measure() != nc->measure())
                                          c->remove(tremolo);
                                    }
                              break;
                              }
                        }
                  }
            }
      }

//---------------------------------------------------------
//   layoutFingering
//    - place numbers above a note execpt for the last
//      staff in a multi stave part (piano)
//    - does not handle chords
//---------------------------------------------------------

void Score::layoutFingering(Fingering* f)
      {
      if (f == 0)
            return;
      TextStyleType tst = f->textStyleType();
      if (tst != TextStyleType::FINGERING && tst != TextStyleType::RH_GUITAR_FINGERING && tst != TextStyleType::STRING_NUMBER)
            return;

      Note* note   = f->note();
      Chord* chord = note->chord();
      Staff* staff = chord->staff();
      Part* part   = staff->part();
      int n        = part->nstaves();
      bool voices  = chord->measure()->hasVoices(staff->idx());
      bool below   = voices ? !chord->up() : (n > 1) && (staff->rstaff() == n-1);
      bool tight   = voices && !chord->beam();

      f->layout();
      qreal x = 0.0;
      qreal y = 0.0;
      qreal headWidth = note->headWidth();
      qreal headHeight = note->headHeight();
      qreal fh = headHeight;        // TODO: fingering number height

      if (chord->notes().size() == 1) {
            x = headWidth * .5;
            if (below) {
                  // place fingering below note
                  y = fh + spatium() * .4;
                  if (tight) {
                        y += 0.5 * spatium();
                        if (chord->stem())
                              x += 0.5 * spatium();
                        }
                  else if (chord->stem() && !chord->up()) {
                        // on stem side
                        y += chord->stem()->height();
                        x -= spatium() * .4;
                        }
                  }
            else {
                  // place fingering above note
                  y = -headHeight - spatium() * .4;
                  if (tight) {
                        y -= 0.5 * spatium();
                        if (chord->stem())
                              x -= 0.5 * spatium();
                        }
                  else if (chord->stem() && chord->up()) {
                        // on stem side
                        y -= chord->stem()->height();
                        x += spatium() * .4;
                        }
                  }
            }
      else {
            x -= spatium();
            }
      f->setUserOff(QPointF(x, y));
      if (x != 0 && y != 0)
            f->setAutoplace(false);
      }

//---------------------------------------------------------
//   checkDivider
//---------------------------------------------------------

static void checkDivider(bool left, System* s, qreal sdd)
      {
      SystemDivider* divider = left ? s->systemDividerLeft() : s->systemDividerRight();
      if (s->score()->styleB(left ? StyleIdx::dividerLeft : StyleIdx::dividerRight)) {
            if (!divider) {
                  divider = new SystemDivider(s->score());
                  divider->setDividerType(left ? SystemDivider::Type::LEFT : SystemDivider::Type::RIGHT);
                  divider->setGenerated(true);
                  s->add(divider);
                  }
            divider->layout();
            divider->rypos() = divider->height() * .5 + sdd;
            divider->adjustReadPos();
            }
      else if (divider) {
            if (divider->generated()) {
                  s->remove(divider);
                  delete divider;
                  }
            else
                  s->score()->undoRemoveElement(divider);
            }
      }

//---------------------------------------------------------
//   layoutPage
//    restHeight - vertical space which has to be distributed
//                 between systems
//---------------------------------------------------------

static void layoutPage(Page* page, qreal restHeight)
      {
      Score* score = page->score();
      int gaps = 0;
      int nsystems = page->systems().size();
      for (int i = 0; i < nsystems - 1; ++i) {
            System* s1 = page->systems().at(i);
            System* s2 = page->systems().at(i+1);
            if (s1->vbox() || s2->vbox())
                  continue;
            ++gaps;
            }

      if (!gaps || MScore::noVerticalStretch || score->layoutMode() == LayoutMode::SYSTEM) {
            if (score->layoutMode() == LayoutMode::FLOAT) {
                  qreal y = restHeight * .5;
                  for (System* system : page->systems())
                        system->move(QPointF(0.0, y));
                  }
            // remove system dividers
            for (System* s : page->systems()) {
                  SystemDivider* sd = s->systemDividerLeft();
                  if (sd) {
                        s->remove(sd);
                        delete sd;
                        }
                  sd = s->systemDividerRight();
                  if (sd) {
                        s->remove(sd);
                        delete sd;
                        }
                  }
            return;
            }

      const qreal maxDistance = score->styleP(StyleIdx::maxSystemDistance);
      qreal stretch = restHeight / gaps;

      qreal yoff = 0;
      for (int i = 0; i < nsystems - 1; ++i) {
            System* s1 = page->systems().at(i);
            System* s2 = page->systems().at(i+1);
            if (!(s1->vbox() || s2->vbox())) {
                  qreal dist   = (s2->y() + yoff) - (s1->y() + s1->height());
                  qreal offset = stretch;
                  if (dist + stretch > maxDistance) {       // limit stretch
                        offset = maxDistance - dist;
                        if (offset < 0)
                              offset = 0;
                        }

                  yoff += offset;

                  // add / remove system dividers

                  qreal sdd = (s2->y() + yoff - s1->y() - s1->height()) * .5 + s1->height();
                  checkDivider(true,  s1, sdd);
                  checkDivider(false, s1, sdd);
                  }
            s2->rypos() += yoff;
            }
      }

//---------------------------------------------------------
//   Spring
//---------------------------------------------------------

struct Spring {
      int seg;
      qreal stretch;
      qreal fix;
      Spring(int i, qreal s, qreal f) : seg(i), stretch(s), fix(f) {}
      };

typedef std::multimap<qreal, Spring, std::less<qreal> > SpringMap;

//---------------------------------------------------------
//   sff2
//    compute 1/Force for a given Extend
//---------------------------------------------------------

static qreal sff2(qreal width, qreal xMin, const SpringMap& springs)
      {
      if (width <= xMin)
            return 0.0;
      auto i = springs.begin();
      qreal c  = i->second.stretch;
      if (c == 0.0)           //DEBUG
            c = 1.1;
      qreal f = 0.0;
      for (; i != springs.end();) {
            xMin -= i->second.fix;
            f = (width - xMin) / c;
            ++i;
            if (i == springs.end() || f <= i->first)
                  break;
            c += i->second.stretch;
            }
      return f;
      }

//---------------------------------------------------------
//   respace
//---------------------------------------------------------

void Score::respace(std::vector<ChordRest*>* elements)
      {
      ChordRest* cr1 = elements->front();
      ChordRest* cr2 = elements->back();
      int n          = elements->size();
      qreal x1       = cr1->segment()->pos().x();
      qreal x2       = cr2->segment()->pos().x();

      qreal width[n-1];
      int ticksList[n-1];
      int minTick = 100000;

      for (int i = 0; i < n-1; ++i) {
            ChordRest* cr  = (*elements)[i];
            ChordRest* ncr  = (*elements)[i+1];
            width[i]       = cr->shape().minHorizontalDistance(ncr->shape());
            ticksList[i]   = cr->duration().ticks();
            minTick = qMin(ticksList[i], minTick);
            }

      //---------------------------------------------------
      // compute stretches
      //---------------------------------------------------

      SpringMap springs;
      qreal minimum = 0.0;
      for (int i = 0; i < n-1; ++i) {
            qreal w   = width[i];
            int t     = ticksList[i];
            qreal str = 1.0 + 0.865617 * log(qreal(t) / qreal(minTick));
            qreal d   = w / str;

            springs.insert(std::pair<qreal, Spring>(d, Spring(i, str, w)));
            minimum += w;
            }

      //---------------------------------------------------
      //    distribute stretch to elements
      //---------------------------------------------------

      qreal force = sff2(x2 - x1, minimum, springs);
      for (auto i = springs.begin(); i != springs.end(); ++i) {
            qreal stretch = force * i->second.stretch;
            if (stretch < i->second.fix)
                  stretch = i->second.fix;
            width[i->second.seg] = stretch;
            }
      qreal x = x1;
      for (int i = 1; i < n-1; ++i) {
            x += width[i-1];
            ChordRest* cr = (*elements)[i];
            qreal dx = x - cr->segment()->pos().x();
            cr->rxpos() += dx;
            }
      }

//---------------------------------------------------------
//   computeMinWidth
//    return the minimum width of segment list s
//    set the width for all segments
//    set the x position of first segment
//---------------------------------------------------------

qreal Score::computeMinWidth(Segment* s, bool isFirstMeasureInSystem)
      {
      qreal x;

      Shape ls;
      if (s->isChordRestType()) {
            // x = qMax(s->minLeft() + styleP(StyleIdx::minNoteDistance), styleP(StyleIdx::barNoteDistance));
            x = s->minLeft() + styleP(StyleIdx::barNoteDistance);
            }
      else {
            if (isFirstMeasureInSystem)
                  ls.add(QRectF(0.0, -1000000.0, 0.0, 2000000.0));   // left margin
            else
                  ls.add(QRectF(0.0, 0.0, 0.0, spatium() * 4));      // simulated bar line
            x = s->minLeft(ls);

            if (s->isClefType())
                  // x = qMax(x, clefLeftMargin);
                  x += styleP(StyleIdx::clefLeftMargin);
            else if (s->isKeySigType())
                  x = qMax(x, styleP(StyleIdx::keysigLeftMargin));
            else if (s->isTimeSigType())
                  x = qMax(x, styleP(StyleIdx::timesigLeftMargin));
            }

      x += s->extraLeadingSpace().val() * spatium();
      bool isSystemHeader = isFirstMeasureInSystem;

      for (Segment* ss = s; ss;) {
            ss->rxpos() = x;
            Segment* ns = ss->next();
            qreal w;

            if (ns) {
                  if (isSystemHeader && ns->isChordRestType()) {        // this is the system header gap
                        w = ss->minHorizontalDistance(ns, true);
                        isSystemHeader = false;
                        }
                  else
                        w = ss->minHorizontalDistance(ns, false);
#if 1
                  // look back for collisions with previous segments
                  // this is time consuming (ca. +5%) and probably requires more optimization

                  int n = 1;
                  for (Segment* ps = ss;;) {
                        qreal ww;
                        if (ps == s)
                              ww = ns->minLeft(ls) - ss->x();
                        else {
                              ps = ps->prev();
                              if (ps->isChordRestType())
                                    ++n;
                              ww = ps->minHorizontalDistance(ns, false) - (ss->x() - ps->x());
                              }
                        if (ww > w) {
                              // overlap !
                              // distribute extra space between segments ps - ss;
                              // only ChordRest segments get more space
                              // TODO: is there a special case n == 0 ?

                              qreal d = (ww - w) / n;
                              qreal xx = ps->x();
                              for (Segment* s = ps; s != ss;) {
                                    Segment* ns = s->next();
                                    qreal ww    = s->width();
                                    if (s->isChordRestType()) {
                                          ww += d;
                                          s->setWidth(ww);
                                          }
                                    xx += ww;
                                    ns->rxpos() = xx;
                                    s = ns;
                                    }
                              w += d;
                              x = xx;
                              break;
                              }
                        if (ps == s)
                              break;
                        }
#endif
                  }
            else
                  w = ss->isEndBarLineType() ? 0.0 : ss->minRight();
            ss->setWidth(w);
            x += w;
            ss = ns;
            }
      return x;
      }

//---------------------------------------------------------
//   updateBarLineSpans
///   updates bar line span(s) when the number of lines of a staff changes
//---------------------------------------------------------

void Score::updateBarLineSpans(int idx, int linesOld, int linesNew)
      {
      int    nStaves = nstaves();
      Staff* _staff;

      // scan staves and check the destination staff of each bar line span
      // barLineSpan is not changed; barLineFrom and barLineTo are changed if they occur in the bottom half of a staff
      // in practice, a barLineFrom/To from/to the top half of the staff is linked to the staff top line,
      // a barLineFrom/To from/to the bottom half of the staff is linked to staff bottom line;
      // this ensures plainchant and mensurstrich special bar lines keep their relationships to the staff lines.
      // 1-line staves are traited as a special case.

      for(int sIdx = 0; sIdx < nStaves; sIdx++) {
            _staff = staff(sIdx);
            // if this is the modified staff
            if(sIdx == idx) {
                  // if it has no bar line, set barLineTo to a default value
                  if(_staff->barLineSpan() == 0)
                        _staff->setBarLineTo( (linesNew-1) * 2);
                  // if new line count is 1, set default From for 1-line staves
                  else if(linesNew == 1)
                        _staff->setBarLineFrom(BARLINE_SPAN_1LINESTAFF_FROM);
                  // if old line count was 1, set default From for normal staves
                  else if (linesOld == 1)
                        _staff->setBarLineFrom(0);
                  // if barLineFrom was below the staff middle position
                  // raise or lower it to account for new number of lines
                  else if(_staff->barLineFrom() > linesOld - 1)
                        _staff->setBarLineFrom(_staff->barLineFrom() + (linesNew - linesOld)*2);
            }

            // if the modified staff is the destination of the current staff bar span:
            if(sIdx + _staff->barLineSpan() - 1 == idx) {
                  // if new line count is 1, set default To for 1-line staves
                  if(linesNew == 1)
                        _staff->setBarLineTo(BARLINE_SPAN_1LINESTAFF_TO);
                  // if old line count was 1, set default To for normal staves
                  else if (linesOld == 1)
                        _staff->setBarLineTo((linesNew - 1) * 2);
                  // if barLineTo was below its middle position, raise or lower it
                  else if(_staff->barLineTo() > linesOld - 1)
                        _staff->setBarLineTo(_staff->barLineTo() + (linesNew - linesOld)*2);
                  }
            }
      }

//---------------------------------------------------------
//   getEmptyPage
//---------------------------------------------------------

Page* Score::getEmptyPage(LayoutContext& lc)
      {
      Page* page;

      if (lc.curPage >= _pages.size()) {
            page = addPage();
            lc.pageOldSystem = 0;
            }
      else {
            page = _pages[lc.curPage];
            lc.pageOldSystem = page->systems().empty() ? 0 : page->systems().back();
            }
      page->setNo(lc.curPage);
      page->layout();
      qreal x, y;
      if (MScore::verticalOrientation()) {
            x = 0.0;
            y = (lc.curPage == 0) ? 0.0 : _pages[lc.curPage - 1]->pos().y() + page->height() + MScore::verticalPageGap;
            }
      else {
            y = 0.0;
            x = (lc.curPage == 0) ? 0.0 : _pages[lc.curPage - 1]->pos().x()
               + page->width()
               + (((lc.curPage+_pageNumberOffset) & 1) ? MScore::horizontalPageGapOdd : MScore::horizontalPageGapEven);
            }
      ++lc.curPage;
      page->setPos(x, y);
      page->systems().clear();

      return page;
      }

//---------------------------------------------------------
//   getNextSystem
//---------------------------------------------------------

System* Score::getNextSystem(LayoutContext& lc)
      {
      bool isVBox = lc.curMeasure->isVBox();
      System* system;
      if (lc.systemList.empty()) {
            system = new System(this);
            lc.systemOldMeasure = 0;
            }
      else {
            system = lc.systemList.takeFirst();
            lc.systemOldMeasure = system->measures().empty() ? 0 : system->measures().back();
            system->clear();   // remove measures from system
            }
      _systems.append(system);
      if (!isVBox) {
            int nstaves = Score::nstaves();
            for (int i = system->staves()->size(); i < nstaves; ++i)
                  system->insertStaff(i);
            int dn = system->staves()->size() - nstaves;
            for (int i = 0; i < dn; ++i)
                  system->removeStaff(system->staves()->size()-1);
            }
      lc.curSystem = system;
      return system;
      }

//---------------------------------------------------------
//   createMMRest
//    create a multi measure rest from m to lm (inclusive)
//---------------------------------------------------------

void Score::createMMRest(Measure* m, Measure* lm, const Fraction& len)
      {
      int n = 1;
      for (Measure* mm = m->nextMeasure(); mm; mm = mm->nextMeasure()) {
            ++n;
            mm->setMMRestCount(-1);
            if (mm->mmRest())
                  undo(new ChangeMMRest(mm, 0));
            if (mm == lm)
                  break;
            }
      Measure* mmr = m->mmRest();
      if (mmr) {
            if (mmr->len() != len) {
                  Segment* s = mmr->findSegment(Segment::Type::EndBarLine, mmr->endTick());
                  mmr->setLen(len);
                  if (s)
                        s->setTick(mmr->endTick());
                  }
            }
      else {
            mmr = new Measure(this);
            mmr->setLen(len);
            mmr->setTick(m->tick());
            mmr->setPageBreak(lm->pageBreak());
            mmr->setLineBreak(lm->lineBreak());
            undo(new ChangeMMRest(m, mmr));
            }
      mmr->setMMRestCount(n);
      mmr->setNo(m->no());

      Segment* ss = lm->findSegment(Segment::Type::EndBarLine, lm->endTick());
      if (ss) {
            Segment* ds = mmr->undoGetSegment(Segment::Type::EndBarLine, lm->endTick());
            for (int staffIdx = 0; staffIdx < nstaves(); ++staffIdx) {
                  Element* e = ss->element(staffIdx * VOICES);
                  if (e) {
                        if (!ds->element(staffIdx * VOICES)) {
                              Element* ee = e->clone();
                              ee->setParent(ds);
                              undoAddElement(ee);
                              }
                        else {
                              BarLine* bd = toBarLine(ds->element(staffIdx * VOICES));
                              BarLine* bs = toBarLine(e);
                              if (bd->barLineType() != bs->barLineType()) {
                                    undoChangeProperty(bd, P_ID::BARLINE_TYPE, QVariant::fromValue(bs->barLineType()));
                                    undoChangeProperty(bd, P_ID::GENERATED, true);
                                    }
                              }
                        }
                  }
            }

      mmr->setRepeatStart(m->repeatStart() || lm->repeatStart());
      mmr->setRepeatEnd(m->repeatEnd() || lm->repeatEnd());

      ElementList oldList = mmr->takeElements();
      ElementList newList = lm->el();

      for (Element* e : m->el()) {
            if (e->isMarker())
                  newList.push_back(e);
            }
      for (Element* e : newList) {
            bool found = false;
            for (Element* ee : oldList) {
                  if (ee->type() == e->type()) {
                        mmr->add(ee);
                        auto i = std::find(oldList.begin(), oldList.end(), ee);
                        if (i != oldList.end())
                              oldList.erase(i);
                        found = true;
                        break;
                        }
                  }
            if (!found)
                  mmr->add(e->clone());
            }
      for (Element* e : oldList)
            delete e;
      Segment* s = mmr->undoGetSegment(Segment::Type::ChordRest, mmr->tick());
      for (int staffIdx = 0; staffIdx < _staves.size(); ++staffIdx) {
            int track = staffIdx * VOICES;
            if (s->element(track) == 0) {
                  Rest* r = new Rest(this);
                  r->setDurationType(TDuration::DurationType::V_MEASURE);
                  r->setDuration(mmr->len());
                  r->setTrack(track);
                  r->setParent(s);
                  undo(new AddElement(r));
                  }
            }

      //
      // check for clefs
      //
      Segment* cs = lm->findSegment(Segment::Type::Clef, lm->endTick());
      Segment* ns = mmr->findSegment(Segment::Type::Clef, lm->endTick());
      if (cs) {
            if (ns == 0)
                  ns = mmr->undoGetSegment(Segment::Type::Clef, lm->endTick());
            for (int staffIdx = 0; staffIdx < _staves.size(); ++staffIdx) {
                  int track = staffIdx * VOICES;
                  Clef* clef = static_cast<Clef*>(cs->element(track));
                  if (clef) {
                        if (ns->element(track) == 0)
                              ns->add(clef->clone());
                        else {
                              //TODO: check if same clef
                              }
                        }
                  }
            }
      else if (ns)
            undo(new RemoveElement(ns));

      //
      // check for time signature
      //
      cs = m->findSegment(Segment::Type::TimeSig, m->tick());
      ns = mmr->findSegment(Segment::Type::TimeSig, m->tick());
      if (cs) {
            if (ns == 0)
                  ns = mmr->undoGetSegment(Segment::Type::TimeSig, m->tick());
            for (int staffIdx = 0; staffIdx < _staves.size(); ++staffIdx) {
                  int track = staffIdx * VOICES;
                  TimeSig* ts = static_cast<TimeSig*>(cs->element(track));
                  if (ts) {
                        TimeSig* nts = static_cast<TimeSig*>(ns->element(track));
                        if (!nts) {
                              nts = ts->clone();
                              nts->setParent(ns);
                              undo(new AddElement(nts));
                              }
                        else {
                              nts->setSig(ts->sig(), ts->timeSigType());
                              nts->layout();
                              }
                        }
                  }
            }
      else if (ns)
            undo(new RemoveElement(ns));

      //
      // check for ambitus
      //
      cs = m->findSegment(Segment::Type::Ambitus, m->tick());
      ns = mmr->findSegment(Segment::Type::Ambitus, m->tick());
      if (cs) {
            if (ns == 0)
                  ns = mmr->undoGetSegment(Segment::Type::Ambitus, m->tick());
            for (int staffIdx = 0; staffIdx < _staves.size(); ++staffIdx) {
                  int track = staffIdx * VOICES;
                  Ambitus* a = toAmbitus(cs->element(track));
                  if (a) {
                        Ambitus* na = toAmbitus(ns->element(track));
                        if (!na) {
                              na = a->clone();
                              na->setParent(ns);
                              undo(new AddElement(na));
                              }
                        else {
                              na->initFrom(a);
                              na->layout();
                              }
                        }
                  }
            }
      else if (ns)
            undo(new RemoveElement(ns));

      //
      // check for key signature
      //
      cs = m->findSegment(Segment::Type::KeySig, m->tick());
      ns = mmr->findSegment(Segment::Type::KeySig, m->tick());
      if (cs) {
            if (ns == 0)
                  ns = mmr->undoGetSegment(Segment::Type::KeySig, m->tick());
            for (int staffIdx = 0; staffIdx < _staves.size(); ++staffIdx) {
                  int track = staffIdx * VOICES;
                  KeySig* ts  = static_cast<KeySig*>(cs->element(track));
                  KeySig* nts = static_cast<KeySig*>(ns->element(track));
                  if (ts) {
                        if (!nts) {
                              KeySig* nks = ts->clone();
                              nks->setParent(ns);
                              undo(new AddElement(nks));
                              }
                        else {
                              if (!(nts->keySigEvent() == ts->keySigEvent())) {
                                    undo(new ChangeKeySig(nts, ts->keySigEvent(), nts->showCourtesy()));
                                    }
                              }
                        }
                  }
            }
      else if (ns && ns->empty())
            undo(new RemoveElement(ns));

      //
      // check for rehearsal mark etc.
      //
      cs = m->findSegment(Segment::Type::ChordRest, m->tick());
      if (cs) {
            for (Element* e : cs->annotations()) {
                  if (!(e->isRehearsalMark() || e->isTempoText() || e->isHarmony() || e->isStaffText()))
                        continue;

                  bool found = false;
                  for (Element* ee : s->annotations()) {
                        if (ee->type() == e->type() && ee->track() == e->track()) {
                              found = true;
                              break;
                              }
                        }
                  if (!found) {
                        Element* ne = e->linkedClone();
                        ne->setParent(s);
                        undo(new AddElement(ne));
                        }
                  }
            }

      for (Element* e : s->annotations()) {
            if (!(e->isRehearsalMark() || e->isTempoText() || e->isHarmony() || e->isStaffText()))
                  continue;
            bool found = false;
            for (Element* ee : cs->annotations()) {
                  if (ee->type() == e->type() && ee->track() == e->track()) {
                        found = true;
                        break;
                        }
                  }
            if (!found)
                  undo(new RemoveElement(e));
            }

      MeasureBase* nm = _showVBox ? lm->next() : lm->nextMeasure();
      mmr->setNext(nm);
      mmr->setPrev(m->prev());
      }

//---------------------------------------------------------
// validMMRestMeasure
//    return true if this might be a measure in a
//    multi measure rest
//---------------------------------------------------------

static bool validMMRestMeasure(Measure* m)
      {
      if (m->irregular())
            return false;

      int n = 0;
      for (Segment* s = m->first(); s; s = s->next()) {
            for (Element* e : s->annotations()) {
                  if (!(e->isRehearsalMark() || e->isTempoText() || e->isHarmony() || e->isStaffText()))
                        return false;
                  }
            if (s->isChordRestType()) {
                  bool restFound = false;
                  int tracks = m->mstaves().size() * VOICES;
                  for (int track = 0; track < tracks; ++track) {
                        if ((track % VOICES) == 0 && !m->score()->staff(track/VOICES)->show()) {
                              track += VOICES-1;
                              continue;
                              }
                        if (s->element(track))  {
                              if (s->element(track)->type() != Element::Type::REST)
                                    return false;
                              Rest* rest = toRest(s->element(track));
                              if (rest->articulations().size() > 0) // break on fermata
                                    return false;
                              restFound = true;
                              }
                        }
                  if (restFound)
                        ++n;
                  // measure is not empty if there is more than one rest
                  if (n > 1)
                        return false;
                  }
            }
      return true;
      }

//---------------------------------------------------------
//  breakMultiMeasureRest
//    return true if this measure should start a new
//    multi measure rest
//---------------------------------------------------------

static bool breakMultiMeasureRest(Measure* m)
      {
      if (m->breakMultiMeasureRest())
            return true;

      if (m->repeatStart()
         || (m->prevMeasure() && m->prevMeasure()->repeatEnd())
         || (m->prevMeasure() && (m->prevMeasure()->sectionBreak())))
            return true;

      auto sl = m->score()->spannerMap().findOverlapping(m->tick(), m->endTick());
      for (auto i : sl) {
            Spanner* s = i.value;
            if (s->isVolta() && (s->tick() == m->tick() || s->tick2() == m->tick()))
                  return true;
            }

      // break for marker in this measure
      for (Element* e : m->el()) {
            if (e->isMarker()) {
                  Marker* mark = toMarker(e);
                  if (!(mark->textStyle().align() & AlignmentFlags::RIGHT))
                        return true;
                  }
            }

      // break for marker & jump in previous measure
      Measure* pm = m->prevMeasure();
      if (pm) {
            for (Element* e : pm->el()) {
                  if (e->isJump())
                        return true;
                  else if (e->isMarker()) {
                        Marker* mark = static_cast<Marker*>(e);
                        if (mark->textStyle().align() & AlignmentFlags::RIGHT)
                              return true;
                        }
                  }
            }

      // break for end of volta
      auto l = m->score()->spannerMap().findOverlapping(m->tick(), m->endTick());
      for (auto isp : l) {
            Spanner* s = isp.value;
            if (s->isVolta() && (s->tick2() == m->endTick()))
                  return true;
            }

      for (Segment* s = m->first(); s; s = s->next()) {
            for (Element* e : s->annotations()) {
                  if (e->isRehearsalMark() ||
                      e->isTempoText() ||
                      ((e->isHarmony() || e->isStaffText()) && (e->systemFlag() || m->score()->staff(e->staffIdx())->show())))
                        return true;
                  }
            for (int staffIdx = 0; staffIdx < m->score()->nstaves(); ++staffIdx) {
                  if (!m->score()->staff(staffIdx)->show())
                        continue;
                  Element* e = s->element(staffIdx * VOICES);
                  if (!e || e->generated())
                        continue;
                  if (s->isStartRepeatBarLineType())
                        return true;
                  if (s->isType(Segment::Type::KeySig | Segment::Type::TimeSig) && m->tick())
                        return true;
                  if (s->isClefType()) {
                        if (s->tick() != m->endTick() && m->tick())
                              return true;
                        }
                  }
            }
      if (pm) {
            Segment* s = pm->findSegment(Segment::Type::EndBarLine, pm->endTick());
            if (s) {
                  for (int staffIdx = 0; staffIdx < s->score()->nstaves(); ++staffIdx) {
                        BarLine* bl = toBarLine(s->element(staffIdx * VOICES));
                        if (bl) {
                              BarLineType t = bl->barLineType();
                              if (t != BarLineType::NORMAL && t != BarLineType::BROKEN && t != BarLineType::DOTTED && !bl->generated())
                                    return true;
                              else
                                    break;
                              }
                        }
                  }
            if (pm->findSegment(Segment::Type::Clef, m->tick()))
                  return true;
            }
      return false;
      }

//---------------------------------------------------------
//   adjustMeasureNo
//---------------------------------------------------------

int LayoutContext::adjustMeasureNo(MeasureBase* m)
      {
      measureNo += m->noOffset();
      m->setNo(measureNo);
      if (!m->irregular())          // dont count measure
            ++measureNo;
      if (m->sectionBreak())
            measureNo = 0;
      return measureNo;
      }

//---------------------------------------------------------
//   createBeams
//    helper function
//---------------------------------------------------------

void Score::createBeams(Measure* measure)
      {
      bool crossMeasure = styleB(StyleIdx::crossMeasureValues);

      for (int track = 0; track < ntracks(); ++track) {
            Staff* stf = staff(track2staff(track));

            // dont compute beams for invisible staffs and tablature without stems
            if (!stf->show() || (stf->isTabStaff() && stf->staffType()->slashStyle()))
                  continue;

            ChordRest* a1    = 0;      // start of (potential) beam
            Beam* beam       = 0;      // current beam
            Beam::Mode bm    = Beam::Mode::AUTO;
            ChordRest* prev  = 0;
            bool checkBeats  = false;
            Fraction stretch = 1;
            QHash<int, TDuration> beatSubdivision;

            // if this measure is simple meter (actually X/4),
            // then perform a prepass to determine the subdivision of each beat

            beatSubdivision.clear();
            TimeSig* ts = stf->timeSig(measure->tick());
            checkBeats  = false;
            stretch     = ts ? ts->stretch() : 1;

            const Segment::Type st = Segment::Type::ChordRest;
            if (ts && ts->denominator() == 4) {
                  checkBeats = true;
                  for (Segment* s = measure->first(st); s; s = s->next(st)) {
                        ChordRest* mcr = static_cast<ChordRest*>(s->element(track));
                        if (mcr == 0)
                              continue;
                        int beat = ((mcr->rtick() * stretch.numerator()) / stretch.denominator()) / MScore::division;
                        if (beatSubdivision.contains(beat))
                              beatSubdivision[beat] = qMin(beatSubdivision[beat], mcr->durationType());
                        else
                              beatSubdivision[beat] = mcr->durationType();
                        }
                  }

            for (Segment* segment = measure->first(st); segment; segment = segment->next(st)) {
                  ChordRest* cr = segment->cr(track);
                  if (cr == 0)
                        continue;
                  for (Lyrics* l : cr->lyrics()) {
                        if (l)
                              l->layout();
                        }

                  // handle grace notes and cross-measure beaming
                  if (cr->isChord()) {
                        Chord* chord = toChord(cr);
                        beamGraceNotes(chord, false); // grace before
                        beamGraceNotes(chord, true);  // grace after
                        // set up for cross-measure values as soon as possible
                        // to have all computations (stems, hooks, ...) consistent with it
                        if (!chord->isGrace())
                              chord->crossMeasureSetup(crossMeasure);
                        }

                  // get defaults from time signature properties
                  bm = Groups::endBeam(cr, prev);

                  // perform additional context-dependent checks
                  if (bm == Beam::Mode::AUTO) {
                        // check if we need to break beams according to minimum duration in current / previous beat
                        if (checkBeats && cr->rtick()) {
                              int tick = (cr->rtick() * stretch.numerator()) / stretch.denominator();
                              // check if on the beat
                              if (tick % MScore::division == 0) {
                                    int beat = tick / MScore::division;
                                    // get minimum duration for this & previous beat
                                    TDuration minDuration = qMin(beatSubdivision[beat], beatSubdivision[beat - 1]);
                                    // re-calculate beam as if this were the duration of current chordrest
                                    TDuration saveDuration        = cr->actualDurationType();
                                    TDuration saveCMDuration      = cr->crossMeasureDurationType();
                                    CrossMeasure saveCrossMeasVal = cr->crossMeasure();
                                    cr->setDurationType(minDuration);
                                    bm = Groups::endBeam(cr, prev);
                                    cr->setDurationType(saveDuration);
                                    cr->setCrossMeasure(saveCrossMeasVal);
                                    cr->setCrossMeasureDurationType(saveCMDuration);
                                    }
                              }
                        }

                  prev = cr;

                  // if chord has hooks and is 2nd element of a cross-measure value
                  // set beam mode to NONE (do not combine with following chord beam/hook, if any)

                  if (cr->durationType().hooks() > 0 && cr->crossMeasure() == CrossMeasure::SECOND)
                        bm = Beam::Mode::NONE;

                  if ((cr->durationType().type() <= TDuration::DurationType::V_QUARTER) || (bm == Beam::Mode::NONE)) {
                        if (beam) {
                              beam->layout1();
                              beam = 0;
                              }
                        if (a1) {
                              a1->removeDeleteBeam(false);
                              a1 = 0;
                              }
                        cr->removeDeleteBeam(false);
                        continue;
                        }

                  if (beam) {
                        bool beamEnd = (bm == Beam::Mode::BEGIN);
                        if (!beamEnd) {
                              cr->removeDeleteBeam(true);
                              beam->add(cr);
                              cr = 0;
                              beamEnd = (bm == Beam::Mode::END);
                              }
                        if (beamEnd) {
                              beam->layout1();
                              beam = 0;
                              }
                        }
                  if (!cr)
                        continue;

                  if (a1 == 0)
                        a1 = cr;
                  else {
                        if (!beamModeMid(bm)
                           &&
                           (bm == Beam::Mode::BEGIN
                              || (a1->segment()->segmentType() != cr->segment()->segmentType())
                              || (a1->tick() + a1->actualTicks() < cr->tick())
                              )
                           )
                              {
                              a1->removeDeleteBeam(false);
                              a1 = cr;
                              }
                        else {
                              beam = a1->beam();
                              if (beam == 0 || beam->elements().front() != a1) {
                                    beam = new Beam(this);
                                    beam->setGenerated(true);
                                    beam->setTrack(track);
                                    a1->removeDeleteBeam(true);
                                    beam->add(a1);
                                    }
                              cr->removeDeleteBeam(true);
                              beam->add(cr);
                              a1 = 0;
                              }
                        }
                  }
            if (beam)
                  beam->layout1();
            else if (a1)
                  a1->removeDeleteBeam(false);
            }
      }

//---------------------------------------------------------
//   getNextMeasure
//---------------------------------------------------------

void Score::getNextMeasure(LayoutContext& lc)
      {
      lc.prevMeasure = lc.curMeasure;
      lc.curMeasure  = lc.nextMeasure;
      if (!lc.curMeasure)
            lc.nextMeasure = _showVBox ? first() : firstMeasure();
      else
            lc.nextMeasure = _showVBox ? lc.curMeasure->next() : lc.curMeasure->nextMeasure();
      if (!lc.curMeasure)
            return;

      int mno       = lc.adjustMeasureNo(lc.curMeasure);
      bool lineMode = _layoutMode == LayoutMode::LINE;

      if (lineMode) {
            while (lc.curMeasure && lc.curMeasure->isVBox()) {
                  lc.curMeasure  = lc.nextMeasure;
                  if (lc.curMeasure)
                        lc.nextMeasure = lc.curMeasure->next();
                  }
            if (!lc.curMeasure)
                  return;
            }
      else if (lc.curMeasure->isMeasure() && score()->styleB(StyleIdx::createMultiMeasureRests)) {
            Measure* m = toMeasure(lc.curMeasure);
            Measure* nm = m;
            Measure* lm = nm;
            int n       = 0;
            Fraction len;

            lc.measureNo = m->no();

            while (validMMRestMeasure(nm)) {
                  MeasureBase* mb = _showVBox ? nm->next() : nm->nextMeasure();
                  if (breakMultiMeasureRest(nm) && n)
                        break;
                  lc.adjustMeasureNo(nm);
                  ++n;
                  len += nm->len();
                  lm = nm;
                  if (!(mb && mb->isMeasure()))
                        break;
                  nm = toMeasure(mb);
                  }
            if (n >= styleI(StyleIdx::minEmptyMeasures)) {
                  createMMRest(m, lm, len);
                  lc.curMeasure  = m->mmRest();
                  lc.nextMeasure = _showVBox ?  lm->next() : lm->nextMeasure();
                  }
            else {
                  if (m->mmRest())
                        undo(new ChangeMMRest(m, 0));
                  m->setMMRestCount(0);
                  lc.measureNo = mno;
                  }
            }
      else if (lc.curMeasure->isMeasure() && toMeasure(lc.curMeasure)->isMMRest()) {
            qDebug("mmrest: no %d += %d", lc.measureNo, toMeasure(lc.curMeasure)->mmRestCount());
            lc.measureNo += toMeasure(lc.curMeasure)->mmRestCount() - 1;
            }

      if (!lc.curMeasure->isMeasure()) {
            lc.curMeasure->setTick(lc.tick);
            return;
            }

      //-----------------------------------------
      //    process one measure
      //-----------------------------------------

      Measure* measure = toMeasure(lc.curMeasure);
      measure->moveTicks(lc.tick - measure->tick());
      if (isMaster() && !lc.prevMeasure) {
            // this is the first measure of a score
            lc.sig = measure->len();
            tempomap()->clear();
            tempomap()->setTempo(0, 2.0);
            sigmap()->clear();
            sigmap()->add(0, SigEvent(lc.sig,  measure->timesig(), 0));
            }

      //
      //  implement section break rest
      //
      if (measure->sectionBreak() && measure->pause() != 0.0)
            setPause(measure->tick() + measure->ticks(), measure->pause());

      //
      // calculate accidentals and note lines,
      // create stem and set stem direction
      //
      for (int staffIdx = 0; staffIdx < score()->nstaves(); ++staffIdx) {
            AccidentalState as;      // list of already set accidentals for this measure
            Staff* staff = Score::staff(staffIdx);
            as.init(staff->key(measure->tick()));

            for (Segment& segment : measure->segments()) {
                  if (segment.isKeySigType()) {
                        KeySig* ks = toKeySig(segment.element(staffIdx * VOICES));
                        if (!ks)
                              continue;
                        as.init(staff->key(segment.tick()));
                        ks->layout();
                        }
                  else if (segment.isChordRestType()) {
                        int track    = staffIdx * VOICES;
                        int endTrack = track + VOICES;
                        for (int t = track; t < endTrack; ++t) {
                              ChordRest* cr = segment.cr(t);
                              if (cr) {
                                    cr->layout0(&as);
                                    cr->layoutArticulations();
                                    }
                              }
                        }
                  else if (segment.isType(Segment::Type::Clef | Segment::Type::TimeSig | Segment::Type::Ambitus)) {
                        Element* e = segment.element(staffIdx * VOICES);
                        if (e)
                              e->layout();
                        }
                  }
            }

      createBeams(measure);

      for (int staffIdx = 0; staffIdx < score()->nstaves(); ++staffIdx) {
            for (Segment& segment : measure->segments()) {
                  if (segment.isChordRestType())
                        layoutChords1(&segment, staffIdx);
                  }
            }

      for (Segment& segment : measure->segments()) {
            if (segment.isBreathType()) {
                  qreal length = 0.0;
                  int tick = segment.tick();
                  // find longest pause
                  for (int i = 0, n = ntracks(); i < n; ++i) {
                        Element* e = segment.element(i);
                        if (e && e->isBreath()) {
                              Breath* b = toBreath(e);
                              b->layout();
                              length = qMax(length, b->pause());
                              }
                        }
                  if (length != 0.0)
                        setPause(tick, length);
                  }
            else if (segment.isTimeSigType()) {
                  for (int staffIdx = 0; staffIdx < _staves.size(); ++staffIdx) {
                        TimeSig* ts = toTimeSig(segment.element(staffIdx * VOICES));
                        if (ts)
                              staff(staffIdx)->addTimeSig(ts);
                        }
                  }
            else if (isMaster() && segment.isChordRestType()) {
                  for (Element* e : segment.annotations()) {
                        if (!(e->isTempoText() || e->isDynamic() || e->isRehearsalMark() || e->isStaffText()))
                              e->layout();
                        }
                  // TODO, this is not going to work, we just cleaned the tempomap
                  // it breaks the test midi/testBaroqueOrnaments.mscx where first note has stretch 2
                  // Also see fixTicks
                  qreal stretch = 0.0;
                  for (Element* e : segment.elist()) {
                        if (!e)
                              continue;
                        ChordRest* cr = toChordRest(e);
                        for (Articulation* a : cr->articulations())
                              stretch = qMax(a->timeStretch(), stretch);
                        if (stretch != 0.0 && stretch != 1.0) {
                              qreal otempo = tempomap()->tempo(cr->tick());
                              qreal ntempo = otempo / stretch;
                              setTempo(cr->tick(), ntempo);
                              int etick = cr->tick() + cr->actualTicks() - 1;
                              auto e = tempomap()->find(etick);
                              if (e == tempomap()->end())
                                    setTempo(etick, otempo);
                              break;
                              }
                        }
                  }
            }

      // update time signature map
      // create event if measure len and time signature are different
      // even if they are equivalent 4/4 vs 2/2
      // also check if nominal time signature has changed

      if (isMaster() && (!measure->len().identical(lc.sig)
                        || (lc.prevMeasure && lc.prevMeasure->isMeasure()
                            && !measure->timesig().identical(toMeasure(lc.prevMeasure)->timesig())))) {
            lc.sig = measure->len();
            sigmap()->add(lc.tick, SigEvent(lc.sig, measure->timesig(), measure->no()));
            }

      for (Segment& s : measure->segments()) {
            // DEBUG: relayout grace notes as beaming/flags may have changed
            if (s.isChordRestType()) {
                  for (Element* e : s.elist()) {
                        if (e && e->isChord()) {
                              Chord* chord = toChord(e);
                              chord->layout();
                              if (chord->tremolo())            // debug
                                    chord->tremolo()->layout();
                              }
                        }
                  }
            else if (s.isEndBarLineType())
                  continue;
            s.createShapes();
            }

      lc.tick += measure->ticks();
      }

//---------------------------------------------------------
//   isTopBeam
//---------------------------------------------------------

static bool isTopBeam(ChordRest* cr)
      {
      if (cr->beam() && cr->beam()->elements().front() == cr) {
            Beam* b = cr->beam();
            bool movedUp = true;
            for (ChordRest* cr1 : b->elements()) {
                  if (cr1->staffMove() >= 0) {
                        movedUp = false;
                        break;
                        }
                  }
            if (!b->cross() && !movedUp)
                  return true;
            }
      return false;
      }

//---------------------------------------------------------
//   notTopBeam
//---------------------------------------------------------

static bool notTopBeam(ChordRest* cr)
      {
      if (cr->beam() && cr->beam()->elements().front() == cr) {
            Beam* b = cr->beam();
            if (b->cross())
                  return true;
            bool movedUp = true;
            for (ChordRest* cr1 : b->elements()) {
                  if (cr1->staffMove() >= 0) {
                        movedUp = false;
                        break;
                        }
                  }
            if (movedUp)
                  return true;
            }
      return false;
      }

//---------------------------------------------------------
//   findLyricsMaxY
//---------------------------------------------------------

static qreal findLyricsMaxY(Segment& s, int staffIdx)
      {
      qreal yMax = 0.0;
      if (!s.isChordRestType())
            return yMax;
      ChordRest* cr = s.cr(staffIdx * VOICES);
      if (cr) {
            Shape sh;
            for (Lyrics* l : cr->lyrics()) {
                  if (l->autoplace() && l->placement() == Element::Placement::BELOW) {
                        l->rUserYoffset() = 0.0;
                        sh.add(l->bbox().translated(l->pos()));
                        }
                  }
            // lyrics shapes must be moved, so first remove them from segment
            s.staffShape(staffIdx).remove(sh);

            qreal lyricsMinTopDistance = s.score()->styleP(StyleIdx::lyricsMinTopDistance);
            for (Lyrics* l : cr->lyrics()) {
                  if (l->autoplace() && l->placement() == Element::Placement::BELOW) {
                        qreal y = s.staffShape(staffIdx).minVerticalDistance(sh);
                        if (y > -lyricsMinTopDistance)
                              yMax = qMax(yMax, y + lyricsMinTopDistance);
                        }
                  }
            }
      return yMax;
      }

static qreal findLyricsMaxY(Measure* m, int staffIdx)
      {
      qreal yMax = 0.0;
      for (Segment& s : m->segments())
            yMax = qMax(yMax, findLyricsMaxY(s, staffIdx));
      return yMax;
      }

//---------------------------------------------------------
//   findLyricsMinY
//---------------------------------------------------------

static qreal findLyricsMinY(Segment& s, int staffIdx)
      {
      qreal yMin = 0.0;
      if (!s.isChordRestType())
            return yMin;
      ChordRest* cr = s.cr(staffIdx * VOICES);
      if (cr) {
            Shape sh;
            for (Lyrics* l : cr->lyrics()) {
                  if (l->autoplace() && l->placement() == Element::Placement::ABOVE) {
                        l->rUserYoffset() = 0.0;
                        sh.add(l->bbox().translated(l->pos()));
                        }
                  }
            // lyrics shapes must be moved, so first remove them from segment
            s.staffShape(staffIdx).remove(sh);

            qreal lyricsMinTopDistance = s.score()->styleP(StyleIdx::lyricsMinTopDistance);
            for (Lyrics* l : cr->lyrics()) {
                  if (l->autoplace() && l->placement() == Element::Placement::ABOVE) {
                        qreal y = sh.minVerticalDistance(s.staffShape(staffIdx));
                        if (y > -lyricsMinTopDistance)
                              yMin = qMin(yMin, -y -lyricsMinTopDistance);
                        }
                  }
            }
      return yMin;
      }

static qreal findLyricsMinY(Measure* m, int staffIdx)
      {
      qreal yMin = 0.0;
      for (Segment& s : m->segments())
            yMin = qMin(yMin, findLyricsMinY(s, staffIdx));
      return yMin;
      }

//---------------------------------------------------------
//   applyLyricsMax
//---------------------------------------------------------

static void applyLyricsMax(Segment& s, int staffIdx, qreal yMax)
      {
      if (!s.isChordRestType())
            return;
      ChordRest* cr = s.cr(staffIdx * VOICES);
      if (cr) {
            Shape sh;
            qreal lyricsMinBottomDistance = s.score()->styleP(StyleIdx::lyricsMinBottomDistance);
            for (Lyrics* l : cr->lyrics()) {
                  if (l->autoplace() && l->placement() == Element::Placement::BELOW) {
                        l->rUserYoffset() = yMax;
                        sh.add(l->bbox().translated(l->pos())
                           .adjusted(0.0, 0.0, 0.0, lyricsMinBottomDistance));
                        }
                  }
            s.staffShape(staffIdx).add(sh);
            }
      }

static void applyLyricsMax(Measure* m, int staffIdx, qreal yMax)
      {
      for (Segment& s : m->segments())
            applyLyricsMax(s, staffIdx, yMax);
      }

//---------------------------------------------------------
//   applyLyricsMin
//---------------------------------------------------------

static void applyLyricsMin(Segment& s, int staffIdx, qreal yMin)
      {
      if (!s.isChordRestType())
            return;
      ChordRest* cr = s.cr(staffIdx * VOICES);
      if (cr) {
            Shape sh;
            qreal lyricsMinBottomDistance = s.score()->styleP(StyleIdx::lyricsMinBottomDistance);
            for (Lyrics* l : cr->lyrics()) {
                  if (l->autoplace() && l->placement() == Element::Placement::ABOVE) {
                        l->rUserYoffset() = yMin;
                        sh.add(l->bbox().translated(l->pos())
                           .adjusted(0.0, -lyricsMinBottomDistance, 0.0, 0.0));
                        }
                  }
            s.staffShape(staffIdx).add(sh);
            }
      }

static void applyLyricsMin(Measure* m, int staffIdx, qreal yMax)
      {
      for (Segment& s : m->segments())
            applyLyricsMin(s, staffIdx, yMax);
      }

//---------------------------------------------------------
//   collectSystem
//---------------------------------------------------------

System* Score::collectSystem(LayoutContext& lc)
      {
      if (!lc.curMeasure) {
            lc.curSystem = 0;
            return 0;
            }
      bool raggedRight = MScore::noHorizontalStretch;
      System* system   = getNextSystem(lc);
      system->setInstrumentNames(lc.startWithLongNames);

      qreal xo;
      if (lc.curMeasure->isHBox())
            xo = point(toHBox(lc.curMeasure)->boxWidth());
      else
            xo = 0;
      system->layoutSystem(xo);

      qreal minMeasureWidth = styleP(StyleIdx::minMeasureWidth);
      qreal minWidth        = system->leftMargin();
      bool firstMeasure     = true;
      qreal measureSpacing  = styleD(StyleIdx::measureSpacing);
      qreal systemWidth     = pageFormat()->printableWidth() * DPI;
      bool lineMode         = _layoutMode == LayoutMode::LINE;

      while (lc.curMeasure) {    // collect measure for system
            System* oldSystem = lc.curMeasure->system();
            lc.curMeasure->setSystem(system);
            system->measures().push_back(lc.curMeasure);

            qreal cautionaryW = 0.0;
            qreal ww          = 0.0;

            if (lc.curMeasure->isHBox())
                  ww = point(toHBox(lc.curMeasure)->boxWidth());
            else if (lc.curMeasure->isMeasure()) {
                  Measure* m = toMeasure(lc.curMeasure);

                  if (firstMeasure) {
                        firstMeasure = false;
                        addSystemHeader(m, lc.firstSystem);
                        ww = computeMinWidth(m->first(), true);
                        }
                  else if (m->hasSystemHeader())
                        ww = m->minWidth1();    // without system header
                  else
                        ww = computeMinWidth(m->first(), false);
                  ww += m->createEndBarLines(true);

                  qreal stretch = m->userStretch() * measureSpacing;
                  if (stretch < 1.0)
                        stretch = 1.0;
                  ww *= stretch;
                  if (ww < minMeasureWidth)
                        ww = minMeasureWidth;
                  m->setWidth(ww);

                  bool hasCourtesy;
                  cautionaryW = cautionaryWidth(m, hasCourtesy) * stretch;

                  // if measure does not already have courtesy elements,
                  // add in the amount of space that courtesy elements would take if needed
                  // (if measure *does* already have courtesy elements, these are included in width already)

                  if (!hasCourtesy)
                        ww += cautionaryW;
                  }

            // check if lc.curMeasure fits, remove if not
            // collect at least one measure

            if (!lineMode && (system->measures().size() > 1) && (minWidth + ww > systemWidth)) {
                  system->measures().pop_back();
                  lc.curMeasure->setSystem(oldSystem);
                  break;
                  }

            if (lc.prevMeasure && lc.prevMeasure->isMeasure() && lc.prevMeasure->system() == system) {
                  Measure* m = toMeasure(lc.prevMeasure);
                  qreal v    = m->createEndBarLines(false);
                  qreal stretch = m->userStretch() * measureSpacing;
                  if (stretch < 1.0)
                        stretch = 1.0;
                  ww += v * stretch;
                  }

            bool pbreak;
            switch (_layoutMode) {
                  case LayoutMode::PAGE:
                  case LayoutMode::SYSTEM:
                        pbreak = lc.curMeasure->pageBreak()
                                 || lc.curMeasure->lineBreak()
                                 || lc.curMeasure->sectionBreak()
                                 || lc.curMeasure->isVBox()
                                 || lc.curMeasure->isTBox();
                        break;
                  case LayoutMode::FLOAT:
                  case LayoutMode::LINE:
                  default:
                        pbreak = false;
                        break;
                  }
            if (lc.rangeLayout && lc.endTick < lc.curMeasure->tick()) {
                  // TODO: we may check if another measure fits in this system
                  if (lc.curMeasure == lc.systemOldMeasure) {
                        lc.rangeDone = true;
                        break;
                        }
                  }
            getNextMeasure(lc);
            minWidth += ww;

            Element::Type nt = lc.curMeasure ? lc.curMeasure->type() : Element::Type::INVALID;
            if (!lineMode
               && (pbreak || nt == Element::Type::VBOX || nt == Element::Type::TBOX || nt == Element::Type::FBOX
                 || (minWidth + minMeasureWidth > systemWidth))) {
                  break;      // break system
                  }

            // whether the measure actually has courtesy elements or whether we added space for hypothetical ones,
            // we should remove the width of courtesy elements for this measure from the accumulated total
            // since at this point we are assuming we may be able to fit another measure
            minWidth -= cautionaryW;
            }     // end collect measures for system

      if (!system->vbox()) {
            if (lc.prevMeasure && lc.prevMeasure->isMeasure())
                  toMeasure(lc.prevMeasure)->createEndBarLines(true);
            system->removeGeneratedElements();
            hideEmptyStaves(system, lc.firstSystem);
            }
      //
      // dont stretch last system row, if accumulated minWidth is <= lastSystemFillLimit
      //
      if (lc.curMeasure == 0 && ((minWidth / systemWidth) <= styleD(StyleIdx::lastSystemFillLimit)))
            raggedRight = true;

      if (!lineMode) {
            //-------------------------------------------------------
            //    add cautionary time/key signatures if needed
            //-------------------------------------------------------

            Measure* m = system->lastMeasure();
            Measure* nm = m ? m->nextMeasure() : 0;
            Segment* s;

            if (m && nm) {
                  m->setHasSystemTrailer(false);
                  int tick = m->endTick();
                  bool isFinalMeasureOfSection = m->isFinalMeasureOfSection();

                  // locate a time sig. in the next measure and, if found,
                  // check if it has court. sig. turned off
                  TimeSig* ts;
                  Segment* tss         = nm->findSegment(Segment::Type::TimeSig, tick);
                  bool showCourtesySig = tss && styleB(StyleIdx::genCourtesyTimesig) && !(isFinalMeasureOfSection && _layoutMode != LayoutMode::FLOAT);

                  if (showCourtesySig) {
                        ts = toTimeSig(tss->element(0));
                        if (ts && !ts->showCourtesySig())
                              showCourtesySig = false;     // this key change has court. sig turned off
                        }
                  if (showCourtesySig) {
                        // if due, create a new courtesy time signature for each staff
                        m->setHasSystemTrailer(true);
                        s  = m->undoGetSegment(Segment::Type::TimeSigAnnounce, tick);
                        int nstaves = Score::nstaves();
                        for (int track = 0; track < nstaves * VOICES; track += VOICES) {
                              TimeSig* nts = toTimeSig(tss->element(track));
                              if (!nts)
                                    continue;
                              ts = toTimeSig(s->element(track));
                              if (!ts) {
                                    ts = new TimeSig(this);
                                    ts->setTrack(track);
                                    ts->setGenerated(true);
                                    ts->setParent(s);
                                    undoAddElement(ts);
                                    }
                              ts->setFrom(nts);
                              ts->layout();
                              s->createShape(track / VOICES);
                              }
                        }
                  else {
                        // remove any existing time signatures
                        Segment* tss = m->findSegment(Segment::Type::TimeSigAnnounce, tick);
                        if (tss)
                              undoRemoveElement(tss);
                        }

                  // courtesy key signatures
                  int n     = _staves.size();
                  bool show = m->hasCourtesyKeySig();
                  Segment* s;
                  if (show)
                        s = m->undoGetSegment(Segment::Type::KeySigAnnounce, tick);
                  else
                        s = m->findSegment(Segment::Type::KeySigAnnounce, tick);

                  Segment* clefSegment = m->findSegment(Segment::Type::Clef, tick);

                  for (int staffIdx = 0; staffIdx < n; ++staffIdx) {
                        int track    = staffIdx * VOICES;
                        Staff* staff = _staves[staffIdx];

                        if (show) {
                              m->setHasSystemTrailer(true);
                              KeySig* ks = toKeySig(s->element(track));
                              KeySigEvent key2 = staff->keySigEvent(tick);

                              if (!ks) {
                                    ks = new KeySig(this);
                                    ks->setKeySigEvent(key2);
                                    ks->setTrack(track);
                                    ks->setGenerated(true);
                                    ks->setParent(s);
                                    undoAddElement(ks);
                                    }
                              else if (!(ks->keySigEvent() == key2)) {
                                    undo(new ChangeKeySig(ks, key2, ks->showCourtesy()));
                                    }
                              ks->layout();
                              s->createShape(track / VOICES);
                              }
                        else {
                              // remove any existent courtesy key signature
                              if (s && s->element(track))
                                    undoRemoveElement(s->element(track));
                              }
                        if (clefSegment) {
                              Clef* clef = toClef(clefSegment->element(track));
                              if (clef && (!score()->styleB(StyleIdx::genCourtesyClef)
                                 || m->repeatEnd() || m->isFinalMeasureOfSection()
                                 || !clef->showCourtesy())) {
                                    clef->clear();          // make invisible
                                    }
                              }
                        }
                  //HACK to layout cautionary elements:
                  if (m->hasSystemTrailer())
                        computeMinWidth(m->first(), false);
                  }
            system->setWidth(systemWidth);
            }

      minWidth           = system->leftMargin();
      qreal totalWeight  = 0.0;
      for (MeasureBase* mb : system->measures()) {
            if (mb->isHBox())
                  minWidth += point(static_cast<Box*>(mb)->boxWidth());
            else if (mb->isMeasure()) {
                  Measure* m    = toMeasure(mb);
                  minWidth     += m->width();
                  qreal stretch = m->userStretch();
                  if (stretch < 1.0)
                        stretch = 1.0;
                  totalWeight  += m->ticks() * stretch;
                  }
            }

      // stretch incomplete row
      qreal rest;
      if (lineMode || MScore::noHorizontalStretch)
            rest = 0;
      else {
            rest = systemWidth - minWidth;
            if (raggedRight) {
                  if (minWidth > rest)
                        rest = rest * .5;
                  else
                        rest = minWidth;
                  }
            rest /= totalWeight;
            }

      QPointF pos(system->leftMargin(), 0.0);
      for (MeasureBase* mb : system->measures()) {
            qreal ww = 0.0;
            if (mb->isMeasure()) {
                  mb->setPos(pos);
                  Measure* m = toMeasure(mb);
                  qreal stretch = m->userStretch();
                  if (stretch < 1.0)
                        stretch = 1.0;
                  if (!lineMode) {
                        ww  = m->width() + rest * m->ticks() * stretch;
                        m->stretchMeasure(ww);
                        }
                  else {
                        m->stretchMeasure(m->width());
                        ww = m->width();
                        }
                  for (MStaff* ms : m->mstaves())
                        ms->lines->layout();
                  }
            else if (mb->isHBox()) {
                  mb->setPos(pos);
                  ww = point(toHBox(mb)->boxWidth());
                  mb->layout();
                  }
            else if (mb->isVBox()) {
                  mb->setPos(pos);
                  }
            pos.rx() += ww;
            }
      if (lineMode)
            system->setWidth(pos.x());

      //
      // layout
      //    - beams
      //    - TempoText
      //    - RehearsalMark, StaffText
      //    - Dynamic
      //    - update the segment shape
      //
      //
      int stick = -1;
      int etick = -1;
      for (MeasureBase* mb : system->measures()) {
            if (!mb->isMeasure())
                  continue;
            if (stick == -1)
                  stick = mb->tick();
            etick = mb->endTick();
            for (Segment* s = toMeasure(mb)->first(Segment::Type::ChordRest); s; s = s->next(Segment::Type::ChordRest)) {
                  for (Element* e : s->elist()) {
                        if (e && e->isChordRest()) {
                              ChordRest* cr = toChordRest(e);
                              if (isTopBeam(cr)) {
                                    cr->beam()->layout();
                                    s->staffShape(cr->staffIdx()).add(cr->beam()->shape().translated(-(cr->segment()->pos()+mb->pos())));
                                    }
                              }
                        }
                  for (Element* e : s->annotations()) {
                        if (e->isTempoText()) {
                              TempoText* tt = toTempoText(e);
                              setTempo(tt->segment(), tt->tempo());
                              tt->layout();
                              if (e->visible())
                                    s->staffShape(tt->staffIdx()).add(tt->shape());
                              }
                        else if (e->visible() && (e->isRehearsalMark() || e->isStaffText())) {
                              e->layout();
                              s->staffShape(e->staffIdx()).add(e->shape());
                              }
                        else if (e->visible() && e->isDynamic()) {
                              Dynamic* d = toDynamic(e);
                              d->layout();

                              if (d->autoplace()) {
                                    // If dynamic is at start or end of a hairpin
                                    // don't autoplace. This is done later on layout of hairpin
                                    // and allows horizontal alignment of dynamic and hairpin.

                                    int tick = d->tick();
                                    auto si = score()->spannerMap().findOverlapping(tick, tick);
                                    bool doAutoplace = true;
                                    for (auto is : si) {
                                          Spanner* sp = is.value;
                                          sp->computeStartElement();
                                          sp->computeEndElement();

                                          if (sp->isHairpin()
                                             && (lookupDynamic(sp->startElement()) == d
                                             || lookupDynamic(sp->endElement()) == d))
                                                doAutoplace = false;
                                          }
                                    if (doAutoplace) {
                                          d->doAutoplace();
                                          d->segment()->staffShape(d->staffIdx()).add(d->shape());
                                          }
                                    }
                              }
                        }
                  }
            }
      //
      //    vertical align lyrics
      //

      VerticalAlignRange ar = VerticalAlignRange(styleI(StyleIdx::autoplaceVerticalAlignRange));

      switch (ar) {
            case VerticalAlignRange::MEASURE:
                  for (MeasureBase* mb : system->measures()) {
                        if (!mb->isMeasure())
                              continue;
                        Measure* m = toMeasure(mb);
                        for (int staffIdx = 0; staffIdx < nstaves(); ++staffIdx) {
                              qreal yMax = findLyricsMaxY(m, staffIdx);
                              applyLyricsMax(m, staffIdx, yMax);
                              }
                        }
                  break;
            case VerticalAlignRange::SYSTEM:
                  for (int staffIdx = 0; staffIdx < nstaves(); ++staffIdx) {
                        qreal yMax = 0.0;
                        qreal yMin = 0.0;
                        for (MeasureBase* mb : system->measures()) {
                              if (!mb->isMeasure())
                                    continue;
                              yMax = qMax(yMax, findLyricsMaxY(toMeasure(mb), staffIdx));
                              yMin = qMin(yMin, findLyricsMinY(toMeasure(mb), staffIdx));
                              }
                        for (MeasureBase* mb : system->measures()) {
                              if (!mb->isMeasure())
                                    continue;
                              applyLyricsMax(toMeasure(mb), staffIdx, yMax);
                              applyLyricsMin(toMeasure(mb), staffIdx, yMin);
                              }
                        }
                  break;
            case VerticalAlignRange::SEGMENT:
                  for (MeasureBase* mb : system->measures()) {
                        if (!mb->isMeasure())
                              continue;
                        Measure* m = toMeasure(mb);
                        for (int staffIdx = 0; staffIdx < nstaves(); ++staffIdx) {
                              for (Segment& s : m->segments()) {
                                    qreal yMax = findLyricsMaxY(s, staffIdx);
                                    applyLyricsMax(s, staffIdx, yMax);
                                    }
                              }
                        }
                  break;
            }

      //
      // compute shape of measures
      //

      for (int si = 0; si < score()->nstaves(); ++si) {
            for (MeasureBase* mb : system->measures()) {
                  if (!mb->isMeasure())
                        continue;
                  Measure* m = toMeasure(mb);
                  m->staffShape(si).clear();
                  for (Segment& s : m->segments())
                        m->staffShape(si).add(s.staffShape(si).translated(s.pos()));
                  m->staffShape(si).add(m->mstaff(si)->lines->bbox());
                  }
            }

      //
      //    layout SpannerSegments for current system
      //

      if (etick > stick) {    // ignore vbox
            auto spanners = score()->spannerMap().findOverlapping(stick, etick);
            std::vector<SpannerSegment*> voltaSegments;
            for (auto interval : spanners) {
                  Spanner* sp = interval.value;
                  if (sp->tick() < etick && sp->tick2() > stick) {
                        if (sp->isOttava() && sp->ticks() == 0) {       // sanity check?
                              sp->setTick2(lastMeasure()->endTick());
                              sp->staff()->updateOttava();
                              }
                        SpannerSegment* ss = sp->layoutSystem(system);     // create/layout spanner segment for this system
                        if (ss->isVoltaSegment() && ss->autoplace())
                              voltaSegments.push_back(ss);
                        }
                  }
            //
            // vertical align volta segments
            //
            if (voltaSegments.size() > 1) {
                  qreal y = 0;
                  for (SpannerSegment* ss : voltaSegments)
                        y = qMin(y, ss->userOff().y());
                  for (SpannerSegment* ss : voltaSegments)
                        ss->setUserYoffset(y);
                  }
            for (Spanner* sp : _unmanagedSpanner) {
                  if (sp->tick() >= etick || sp->tick2() < stick)
                        continue;
                  sp->layout();
                  }

            //
            // add SpannerSegment shapes to staff shapes
            //

            for (MeasureBase* mb : system->measures()) {
                  if (!mb->isMeasure())
                        continue;
                  Measure* m = toMeasure(mb);
                  for (SpannerSegment* ss : system->spannerSegments()) {
                        Spanner* sp = ss->spanner();
                        if (sp->tick() < m->endTick() && sp->tick2() > m->tick()) {
                              // spanner shape must be translated from system coordinate space to measure coordinate space
                              if (ss->type() == Element::Type::LYRICSLINE_SEGMENT) {
                                    m->staffShape(sp->staffIdx()).add(ss->shape().translated(-m->pos()));
                                    }
                              else {
                                    m->staffShape(sp->staffIdx()).add(ss->shape().translated(ss->pos() - m->pos()));
                                    }
                              }
                        }
                  }
            }
      system->layout2();   // compute staff distances

      Measure* lm  = system->lastMeasure();
      if (lm) {
            lc.firstSystem        = lm->sectionBreak() && _layoutMode != LayoutMode::FLOAT;
            lc.startWithLongNames = lc.firstSystem && lm->sectionBreak()->startWithLongNames();
            }
      lc.systemChanged      = lc.systemOldMeasure != (system->measures().empty() ? 0 : system->measures().back());
      return system;
      }

//---------------------------------------------------------
//   collectPage
//---------------------------------------------------------

bool Score::collectPage(LayoutContext& lc)
      {
      if (!lc.curSystem)
            return false;

      const qreal slb = styleP(StyleIdx::staffLowerBorder);
      const qreal sub = styleP(StyleIdx::staffUpperBorder);
      bool breakPages = layoutMode() != LayoutMode::SYSTEM;

      Page* page = getEmptyPage(lc);
      qreal y    = page->tm();
      qreal ey   = page->height() - page->bm();
      System* s1 = 0;               // previous system
      System* s2 = lc.curSystem;

      for (;;) {
            //
            // calculate distance to previous system
            //
            qreal distance;
            if (s1) {
                  distance = s1->minDistance(s2);
                  }
            else {
                  // this is the first system on page
                  VBox* vbox = s2->vbox();
                  distance = vbox ? vbox->topGap() : sub;
                  distance = qMax(distance, -s2->minTop());
                  }
            distance += _staves.front()->userDist();

            y += distance;
            s2->setPos(page->lm(), y);
            page->appendSystem(s2);
            y += s2->height();

            //
            //  check for page break or if next system will fit on page
            //
            if (lc.rangeDone) {
                  // take next system unchanged
                  System* s    = lc.systemList.empty() ? 0 : lc.systemList.takeFirst();
                  lc.curSystem = s;
                  if (s)
                        _systems.append(lc.curSystem);
                  }
            else {
                  collectSystem(lc);
                  }
            System* s3     = lc.curSystem;
            bool breakPage = !s3 || (breakPages && s2->pageBreak());

            if (!breakPage) {
                  qreal dist = s2->minDistance(s3) + s3->height();
                  VBox* vbox = s3->vbox();
                  if (vbox)
                        dist += vbox->bottomGap();
                  else
                        dist += qMax(s3->minBottom(), slb);
                  breakPage  = (y + dist) >= ey;
                  }
            if (breakPage) {
                  // dont use currentSystem
                  VBox* vbox = s2->vbox();
                  qreal dist = vbox ? vbox->bottomGap() : qMax(s2->minBottom(), slb);
                  layoutPage(page, ey - (y + dist));
                  break;
                  }
            s1 = s2;    // current system becomes previous
            s2 = s3;    // next system becomes current
            }

      int stick = -1;
      int tracks = nstaves() * VOICES;
      for (System* s : page->systems()) {
            for (MeasureBase* mb : s->measures()) {
                  if (!mb->isMeasure())
                        continue;
                  Measure* m = toMeasure(mb);
                  if (stick == -1)
                        stick = m->tick();

                  for (int track = 0; track < tracks; ++track) {
                        for (Segment* segment = m->first(); segment; segment = segment->next()) {
                              Element* e = segment->element(track);
                              if (!e)
                                    continue;
                              if (e->isChordRest()) {
                                    if (!staff(track2staff(track))->show())
                                          continue;
                                    ChordRest* cr = toChordRest(e);
                                    if (notTopBeam(cr))                   // layout cross staff beams
                                          cr->beam()->layout();

                                    if (cr->isChord()) {
                                          Chord* c = toChord(cr);
                                          for (Chord* cc : c->graceNotes()) {
                                                if (cc->beam() && cc->beam()->elements().front() == cc)
                                                      cc->beam()->layout();
                                                for (Note* n : cc->notes()) {
                                                      Tie* tie = n->tieFor();
                                                      if (tie)
                                                            tie->layout();
                                                      for (Spanner* sp : n->spannerFor())
                                                            sp->layout();
                                                      }
                                                for (Element* e : cc->el()) {
                                                      if (e->isSlur())
                                                            e->layout();
                                                      }
                                                }
                                          c->layoutArpeggio2();
                                          for (Note* n : c->notes()) {
                                                Tie* tie = n->tieFor();
                                                if (tie)
                                                      tie->layout();
                                                for (Spanner* sp : n->spannerFor())
                                                      sp->layout();
                                                }
                                          }
                                    }
                              else if (e->isBarLine())
                                    e->layout();
                              }
                        }
                  m->layout2();
                  }
            }
      page->rebuildBspTree();
      lc.pageChanged = lc.systemChanged || (lc.pageOldSystem != (page->systems().empty() ? 0 : page->systems().back()));
      return true;
      }

//---------------------------------------------------------
//   doLayout
//    input:      list of measures
//    output:     list of systems
//                list of pages
//---------------------------------------------------------

void Score::doLayout()
      {
//      qDebug();

      if (_staves.empty() || first() == 0) {
            // score is empty
            _pages.clear();

            Page* page = addPage();
            page->layout();
            page->setNo(0);
            page->setPos(0.0, 0.0);
            page->rebuildBspTree();
            qDebug("layout: empty score");
            return;
            }

      _scoreFont     = ScoreFont::fontFactory(_style.value(StyleIdx::MusicalSymbolFont).toString());
      _noteHeadWidth = _scoreFont->width(SymId::noteheadBlack, spatium() / SPATIUM20);

      if (cmdState().layoutFlags & LayoutFlag::FIX_PITCH_VELO)
            updateVelo();
      if (cmdState().layoutFlags & LayoutFlag::PLAY_EVENTS)
            createPlayEvents();

      LayoutContext lc;
      lc.rangeDone = false;
      _systems.swap(lc.systemList);
      getNextMeasure(lc);
      getNextMeasure(lc);

      collectSystem(lc);
      while (collectPage(lc))
            ;
      if (_layoutMode == LayoutMode::LINE) {
            Page* page = _pages[0];
            page->setWidth(page->system(0)->width());
            }

      // remove not needed systems
      // TODO: make undoable
      for (System* system : lc.systemList) {
            qDebug("delete system");
            for (SpannerSegment* ss : system->spannerSegments()) {
                  qDebug("   delete spanner segment\n");
                  Spanner* spanner = ss->spanner();
                  spanner->spannerSegments().removeOne(ss);
                  }
            }
      // remove not needed pages
      // TODO: make undoable
      while (_pages.size() > lc.curPage)
            _pages.takeLast();

      for (auto s : _spanner.map()) {     // TODO: this invalidates the bsp tree
            Spanner* sp = s.second;
            if (sp->isSlur())
                  sp->layout();
            }

      for (MuseScoreView* v : viewer)
            v->layoutChanged();

      // _mscVersion is used during read and first layout
      // but then it's used for drag and drop and should be set to new version
      _mscVersion = MSCVERSION;     // for later drag & drop usage
#ifndef NDEBUG
      if (MScore::showCorruptedMeasures)
            sanityCheck();
#endif
      }

//---------------------------------------------------------
//   doLayoutRange
//---------------------------------------------------------

void Score::doLayoutRange(int stick, int etick)
      {
//      qDebug("%d-%d", stick, etick);
      if (stick == -1 || etick == -1) {
            doLayout();
            return;
            }
      if (stick < 0)
            stick = 0;
      LayoutContext lc;

#if 0
      if (_layoutMode == LayoutMode::LINE) {
            _systems.swap(lc.systemList);
            getNextMeasure(lc);
            getNextMeasure(lc);
            layoutLinear(lc);
            return;
            }
#endif

      lc.rangeLayout = true;
      lc.rangeDone   = false;
      lc.endTick     = etick;
      _scoreFont     = ScoreFont::fontFactory(_style.value(StyleIdx::MusicalSymbolFont).toString());
      _noteHeadWidth = _scoreFont->width(SymId::noteheadBlack, spatium() / SPATIUM20);

      if (cmdState().layoutFlags & LayoutFlag::FIX_PITCH_VELO)
            updateVelo();
      if (cmdState().layoutFlags & LayoutFlag::PLAY_EVENTS)
            createPlayEvents();

      //---------------------------------------------------
      //    initialize layout context lc
      //---------------------------------------------------

      Measure* m = tick2measure(stick);
      // start layout one measure earlier to handle clefs and cautionary elements
      if (m->prevMeasureMM()) {
            m = m->prevMeasureMM();
            }

      // if the first measure of the score is part of a multi measure rest
      // m->system() will return a nullptr. We need to find the multi measure
      // rest which replaces the measure range
      if (!m->system() && m->hasMMRest())
            m = m->mmRest();
      Q_ASSERT(m->system());

      Page* p    = m->system()->page();
      System* s  = p->systems().front();

      int systemIndex  = _systems.indexOf(s);
      lc.systemList    = _systems.mid(systemIndex);
      _systems.erase(_systems.begin() + systemIndex, _systems.end());
      lc.curPage       = _pages.indexOf(p);
      lc.curSystem     = systemIndex > 0 ? _systems[systemIndex-1] : 0;
      lc.prevMeasure   = 0;
      lc.curMeasure    = s->measure(0)->prev();
      lc.nextMeasure   = s->measure(0);
      lc.measureNo     = lc.nextMeasure->no();
      lc.tick          = lc.nextMeasure->tick();

      getNextMeasure(lc);
      collectSystem(lc);

      //---------------------------------------------------
      //    layout score
      //---------------------------------------------------

      while (collectPage(lc)) {
            Page* page     = _pages[lc.curPage-1];
            System* s      = page->system(0);
            MeasureBase* m = s->measures().back();
            if (lc.rangeDone && m->tick() > etick) {
                  break;
                  }
            }
      if (!lc.curSystem) {
            while (_pages.size() > lc.curPage)        // Remove not needed pages. TODO: make undoable:
                  _pages.takeLast();
            }

      _systems.append(lc.systemList);

      for (MuseScoreView* v : viewer)
            v->layoutChanged();
      }

}
