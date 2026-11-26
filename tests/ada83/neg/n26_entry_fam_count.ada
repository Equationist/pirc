-- Negative test: P3-7.15 unindexed entry family prefix for 'Count
procedure Neg_Entry_Fam_Count is
   task T is
      entry F (1 .. 3);
   end T;
   task body T is
      N : Integer;
   begin
      N := F'Count;
   end T;
begin
   null;
end Neg_Entry_Fam_Count;
