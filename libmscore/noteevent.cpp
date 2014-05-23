//=============================================================================
//  MuseScore
//  Music Composition & Notation
//
//  Copyright (C) 2010-2011 Werner Schweer
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2
//  as published by the Free Software Foundation and appearing in
//  the file LICENCE.GPL
//=============================================================================

#include "noteevent.h"
#include "xml.h"

namespace Ms {

//---------------------------------------------------------
//   read
//---------------------------------------------------------

void NoteEvent::read(XmlReader& e)
      {
      while (e.readNextStartElement()) {
            const QStringRef& tag(e.name());
          if (tag == "noteeventPitch"){
                setProperty(P_PITCH, Ms::getProperty(P_EVENT_PITCH, e));

             //       _pitch = e.readInt();
            }
            else if (tag == "ontime"){
                setProperty(P_ONTIME, Ms::getProperty(P_ONTIME, e));

                //    _ontime = e.readInt();
            }
            else if (tag == "len"){
                setProperty(P_LEN, Ms::getProperty(P_LEN, e));

                //     _len = e.readInt();
            }
            else
                  e.unknown();
            }
      }

//---------------------------------------------------------
//   write
//---------------------------------------------------------

void NoteEvent::write(Xml& xml) const
      {
      xml.stag("Event");
      
      writeProperty(xml, P_EVENT_PITCH);
      writeProperty(xml, P_ONTIME);
      writeProperty(xml, P_LEN);
      
     /* xml.tag("pitch", _pitch);
      xml.tag("ontime", _ontime);
      xml.tag("len", _len); */
      xml.etag();
      }

//---------------------------------------------------------
//   NoteEventList
//---------------------------------------------------------

NoteEventList::NoteEventList()
   : QList<NoteEvent>()
      {
      }

//---------------------------------------------------------
//   operator==
//---------------------------------------------------------

bool NoteEvent::operator==(const NoteEvent& e) const
      {
      return (e._pitch == _pitch) && (e._ontime == _ontime) && (e._len == _len);
      }

}

