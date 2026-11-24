@0xa4f1501f8d663be2;

interface Participant {
  showMessage @0 (nick :Text, text :Text) -> ();
}

interface Session {
  sendMessage @0 (text :Text) -> ();
}

interface Room {
  join @0 (nick :Text, participant :Participant) -> (session :Session, nick :Text);
}
