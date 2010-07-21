/* @file rs_rollback.cpp
* 
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "../client.h"
#include "../../client/dbclient.h"
#include "rs.h"
#include "../repl.h"

/* Scenarios

   We went offline with ops not replicated out.
 
       F = node that failed and coming back.
       P = node that took over, new primary

   #1:
       F : a b c d e f g
       P : a b c d q

   The design is "keep P".  One could argue here that "keep F" has some merits, however, in most cases P 
   will have significantly more data.  Also note that P may have a proper subset of F's stream if there were 
   no subsequent writes.

   For now the model is simply : get F back in sync with P.  If P was really behind or something, we should have 
   just chosen not to fail over anyway.

   #2:
       F : a b c d e f g                -> a b c d
       P : a b c d

   #3:
       F : a b c d e f g                -> a b c d q r s t u v w x z
       P : a b c d.q r s t u v w x z

   Steps
    find an event in common. 'd'.
    undo our events beyond that by: 
      (1) taking copy from other server of those objects
      (2) do not consider copy valid until we pass reach an optime after when we fetched the new version of object 
          -- i.e., reset minvalid.
      (3) we could skip operations on objects that are previous in time to our capture of the object as an optimization.

*/

namespace mongo {

    using namespace bson;

    struct HowToFixUp {
        list<bo> toRefetch;
        OpTime commonPoint;
    };

    static void syncRollbackFindCommonPoint(DBClientConnection *us, DBClientConnection *them, HowToFixUp& h) { 
        const Query q = Query().sort( BSON( "$natural" << -1 ) );
        const bo fields = BSON( "ts" << 1 << "h" << 1 );
        
        auto_ptr<DBClientCursor> u = us->query(rsoplog, q, 0, 0, &fields, 0, 0);
        auto_ptr<DBClientCursor> t = them->query(rsoplog, q, 0, 0, &fields, 0, 0);

        if( !u->more() ) throw "our oplog empty or unreadable";
        if( !t->more() ) throw "remote oplog empty or unreadable";

        BSONObj ourObj = u->nextSafe();
        OpTime ourTime = ourObj["ts"]._opTime();
        BSONObj theirObj = t->nextSafe();
        OpTime theirTime = theirObj["ts"]._opTime();

        if( 1 ) {
            long long diff = (long long) ourTime.getSecs() - ((long long) theirTime.getSecs());
            /* diff could be positive, negative, or zero */
            log() << "replSet TEMP info syncRollback diff in end of log times : " << diff << " seconds" << rsLog;
            /*if( diff > 3600 ) { 
                log() << "replSet syncRollback too long a time period for a rollback. sleeping 1 minute" << rsLog;
                sleepsecs(60);
                throw "error not willing to roll back more than one hour of data";
            }*/
        }

        unsigned long long totSize = 0;
        unsigned long long scanned = 0;
        while( 1 ) {
            scanned++;
            /* todo add code to assure no excessive scanning for too long */
            if( ourTime == theirTime ) { 
                if( ourObj["h"].Long() == theirObj["h"].Long() ) { 
                    // found the point back in time where we match.
                    // todo : check a few more just to be careful about hash collisions.
                    log() << "replSet rollback found matching events at " << ourTime.toStringPretty() << rsLog;
                    log() << "replSet scanned : " << scanned << rsLog;
                    log() << "replSet TODO finish " << rsLog;
                    h.commonPoint = ourTime;
                    return;
                }
                theirObj = t->nextSafe();
                theirTime = theirObj["ts"]._opTime();
                ourObj = u->nextSafe();
                ourTime = ourObj["ts"]._opTime();
            }
            else if( theirTime > ourTime ) { 
                /* todo: we could hit beginning of log here.  exception thrown is ok but not descriptive, so fix up */
                theirObj = t->nextSafe();
                theirTime = theirObj["ts"]._opTime();
            }
            else { 
                // theirTime < ourTime
                totSize += ourObj.objsize();
                if( totSize > 512 * 1024 * 1024 )
                    throw "rollback too large";
                h.toRefetch.push_back( ourObj.getOwned() );
                ourObj = u->nextSafe();
                ourTime = ourObj["ts"]._opTime();
            }
        }
    }

    void ReplSetImpl::syncRollback(OplogReader&r) { 
        assert( !lockedByMe() );
        assert( !dbMutex.atLeastReadLocked() );

        HowToFixUp how;
        sethbmsg("syncRollback 1");
        {
            r.resetCursor();
            DBClientConnection us(false, 0, 0);
            string errmsg;
            if( !us.connect(HostAndPort::me().toString(),errmsg) ) { 
                sethbmsg("syncRollback connect to self failure" + errmsg);
                return;
            }

            sethbmsg("syncRollback 2 FindCommonPoint");
            try {
                syncRollbackFindCommonPoint(&us, r.conn(), how);
            }
            catch( const char *p ) { 
                sethbmsg(string("syncRollback 2 error ") + p);
                sleepsecs(10);
                return;
            }
            catch( DBException& e ) { 
                sethbmsg(string("syncRollback 2 exception ") + e.toString() + "; sleeping 1 min");
                sleepsecs(60);
                throw;
            }
        }

        sethbmsg("replSet syncRollback 3 FINISH");
    }

}