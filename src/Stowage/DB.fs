namespace Stowage
open Stowage.Internal.LMDB
open System.IO

/// Stowage is a key-value database that features garbage collected
/// references between binaries via secure hashes. 
///
/// Stowage is implemented above LMDB, a memory-mapped B-tree. Stowage
/// transactions are optimistic and lightweight, held in memory until
/// commit. Non-conflicting writes are batched to amortize overheads 
/// for synchronizing to disk.
///
/// The ability to reference binaries via secure hashes, together with
/// garbage collection, enables a stowage database to represent larger 
/// than memory persistent data structures in a purely functional style.
/// It also supports simple structure sharing, and should be relatively
/// easy to shard.
module DB =

    module internal Internal =
        type DB =
            { 
                db_lock : FileStream    
                db_env  : MDB_env     
                db_data : MDB_dbi     // user string -> data
                db_stow : MDB_dbi     // secure hash -> data
                db_rfct : MDB_dbi     // hashes with refct > 0
                db_zero : MDB_dbi     // hashes with zero refct
                // ephemeron table
                // task queue
                // concurrency primitives
            }  
        type TX =
            {   tx_db   : DB
                // reads
                // writes
                // ephemeral refs
            }
            interface System.IDisposable with
                member tx.Dispose() =
                    // clear ephemerons
                    ()

    /// Stowage database object (abstract)
    [< Struct >]
    type DB internal (db : Internal.DB) =
        member internal x.Impl = db

    /// Transaction object (abstract)
    [< Struct >]
    type TX internal (tx : Internal.TX) =
        member internal x.Impl = tx
   
    let newTX (db : DB) : TX =
        TX { tx_db = db.Impl
             // other default values
           }
    
    let txDB (tx : TX) : DB = DB (tx.Impl.tx_db)


    // still needed: Task data and synchronization primitives?
    // or maybe a separate thread with Monitor?
 
    // fragment of hash used for stowage keys
    let private stowKeyLen = Hash.validHashLen / 2

    let inline private withDir (p : string) (op : unit -> 'R) : 'R =
        do ignore <| System.IO.Directory.CreateDirectory(p) 
        let p0 = System.IO.Directory.GetCurrentDirectory()
        try 
            do ignore <| System.IO.Directory.SetCurrentDirectory(p)
            op ()
        finally
            System.IO.Directory.SetCurrentDirectory(p0)

    let inline lockFile (fn : string) : FileStream =
        new FileStream(
                fn, 
                FileMode.OpenOrCreate, 
                FileAccess.ReadWrite, 
                FileShare.None,
                8,
                FileOptions.DeleteOnClose)

    // assuming we're in the target directory, build the database
    let inline private mkDB (maxSizeMB : int) () : DB =
        let lock = lockFile ".lock"
        let env = mdb_env_create ()
        mdb_env_set_mapsize env maxSizeMB
        mdb_env_set_maxdbs env 4
        let envFlags = MDB_NOSYNC ||| MDB_WRITEMAP ||| MDB_NOTLS ||| MDB_NOLOCK
        mdb_env_open env "." envFlags
        let txn = mdb_readwrite_txn_begin env
        let dbData = mdb_dbi_open txn "@" MDB_CREATE // root key-value
        let dbStow = mdb_dbi_open txn "$" MDB_CREATE // stowed resources
        let dbRfct = mdb_dbi_open txn "#" MDB_CREATE // positive refcts
        let dbZero = mdb_dbi_open txn "0" MDB_CREATE // keys with zero refct
        mdb_txn_commit txn

        DB { db_lock = lock
             db_env  = env
             db_data = dbData
             db_stow = dbStow
             db_rfct = dbRfct
             db_zero = dbZero
           }
    // TODO: 
    //   ephemerons table 
    //   task queue / batch
    //   init writer thread

    let load (path : string) (maxSizeMB : int) : DB = 
        withDir path (mkDB maxSizeMB)
                

    /// Obtain current reference count for a hash.
    ///   


(*

    , dupTX
    , readKey, readKeyDB
    , readKeys, readKeysDB
    , writeKey, assumeKey
    , loadRsc, loadRscDB
    , withRsc, withRscDB
    , stowRsc
    , clearRsc, clearRsc'
    , commit, commit_async
    , check
    , gcDB, gcDB_async
    , hashDeps
    , FilePath
    , ByteString
    , Hash
    
*)
  
    

